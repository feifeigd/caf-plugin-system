#include "bridge_actor.hpp"

#include "common/message_tags.hpp"
#include "common/plugin_envelope.hpp"
#include "service_registry.hpp"
#include "services/logging_service.hpp"

#include <caf/all.hpp>
#include <caf/actor_registry.hpp>

#include <atomic>
#include <algorithm>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#ifdef _WIN32
#include <winsock2.h> // 必须在 windows.h 之前
#pragma comment(lib, "ws2_32.lib")
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace caf_plugin_system {

namespace {

// ------------------------------------------------------------------
// 行协议常量
// ------------------------------------------------------------------
constexpr std::uint16_t k_bridge_sub_proto = 1; // external_echo 信封子协议号

// ------------------------------------------------------------------
// 跨平台 socket 别名
// ------------------------------------------------------------------
struct BridgeImplState {
    int seq = 0;                            // REQ 序号（rid）
    // 用 typed_response_promise（public 可拷贝）——caf::response_promise
    // 的拷贝构造是 private（friend typed_response_promise），map 存不了。
    std::map<int, caf::typed_response_promise<std::string>> pending;
};

struct BridgeState {}; // bridge 主 actor 无状态（全在 lambda 捕获里）

#ifdef _WIN32
using socket_t = SOCKET;
constexpr socket_t k_invalid_socket = INVALID_SOCKET;
#else
using socket_t = int;
constexpr socket_t k_invalid_socket = -1;
#endif

void close_socket(socket_t fd) {
#ifdef _WIN32
    closesocket(fd);
#else
    ::close(fd);
#endif
}

// ------------------------------------------------------------------
// 线程安全的写队列：写线程消费，actor（bridge / external_echo）生产
// ------------------------------------------------------------------
struct WriteQueue {
    std::mutex m;
    std::condition_variable cv;
    std::deque<std::string> lines;
    bool closed = false;

    void push(std::string line) {
        std::lock_guard<std::mutex> lock(m);
        lines.push_back(std::move(line));
        cv.notify_one();
    }

    // 阻塞取一行；closed 且空时返回 false（写线程退出）
    bool pop(std::string& out) {
        std::unique_lock<std::mutex> lock(m);
        cv.wait(lock, [this] { return closed || !lines.empty(); });
        if (lines.empty())
            return false;
        out = std::move(lines.front());
        lines.pop_front();
        return true;
    }

    void close() {
        std::lock_guard<std::mutex> lock(m);
        closed = true;
        cv.notify_all();
    }
};

// ------------------------------------------------------------------
// 当前活动连接：actor 写 socket 的唯一入口 + 全部资源句柄。
// 坑 1：若 actor 直接捕获"主 wq"，而写线程消费的是每连接新建的 wq，
//       则 respond/REQ 全进无人消费的队列 → 外部进程永远等不到响应
//       （实测：Python 15s 超时、master 跨节点 request_timeout）。
//       因此 actor 必须经 active->push() 转发到【当前连接】的 wq。
// 坑 2：detached 线程在进程退出时被强杀不 unwind → 栈上 shared_ptr
//       不析构 → CRT dump 报泄露（实测 "REQ 53 ..." 残留在 WriteQueue）。
//       因此线程全部登记到 active，bridge actor exit 时 shutdown_and_join：
//       close 所有 fd + wq->close() → 线程阻塞点返回 → join → 0 泄露。
// ------------------------------------------------------------------
struct ActiveConn {
    std::mutex m;
    std::shared_ptr<WriteQueue> wq;
    socket_t listener = k_invalid_socket;
    std::vector<socket_t> conns;
    std::vector<std::thread> threads;
    bool shutting_down = false;

    void set_listener(socket_t s) {
        std::lock_guard<std::mutex> g(m);
        listener = s;
    }
    void set_wq(std::shared_ptr<WriteQueue> w) {
        std::lock_guard<std::mutex> g(m);
        wq = std::move(w);
    }
    void add_conn(socket_t fd) {
        std::lock_guard<std::mutex> g(m);
        conns.push_back(fd);
    }
    void add_thread(std::thread t) {
        std::lock_guard<std::mutex> g(m);
        threads.push_back(std::move(t));
    }
    void push(const std::string& line) {
        std::lock_guard<std::mutex> g(m);
        if (wq)
            wq->push(line);
    }
    bool is_shutting_down() {
        std::lock_guard<std::mutex> g(m);
        return shutting_down;
    }
    void shutdown_and_join() {
        std::vector<std::thread> ts;
        {
            std::lock_guard<std::mutex> g(m);
            if (!shutting_down) {
                shutting_down = true;
                if (listener != k_invalid_socket)
                    close_socket(listener);
                for (auto fd : conns)
                    close_socket(fd);
                if (wq)
                    wq->close();
            }
            ts = std::move(threads);
        }
        for (auto& t : ts)
            if (t.joinable())
                t.join();
    }
};

// ------------------------------------------------------------------
// 带缓冲的行读取器（阻塞 recv）
// ------------------------------------------------------------------
struct LineReader {
    socket_t fd;
    std::string buf;
    std::shared_ptr<ActiveConn> active; // 优雅关闭标志查询（recv 失败门控）

    // 读一行（含 \n）；EOF/错误返回 false
    bool read_line(std::string& out) {
        while (true) {
            auto nl = buf.find('\n');
            if (nl != std::string::npos) {
                out = buf.substr(0, nl + 1);
                buf.erase(0, nl + 1);
                return true;
            }
            char tmp[4096];
            int n = recv(fd, tmp, static_cast<int>(sizeof tmp), 0);
            if (n <= 0) {
                // 优雅关闭时 shutdown_and_join() 主动 close 全部 fd，
                // recv 失败（WSAECONNABORTED）是预期行为，不刷噪音；
                // 非关闭期的失败才打（客户端异常断开，调试有用）。
                if (!active || !active->is_shutting_down()) {
#ifdef _WIN32
                    fprintf(stderr, "[Bridge] recv failed, err=%d n=%d\n",
                            WSAGetLastError(), n);
#else
                    fprintf(stderr, "[Bridge] recv failed, errno=%d n=%d\n",
                            errno, n);
#endif
                    fflush(stderr);
                }
                return false;
            }
            buf.append(tmp, static_cast<size_t>(n));
        }
    }

    // 精确读 len 字节（可能跨缓冲）
    bool read_exact(char* dst, size_t len) {
        while (buf.size() < len) {
            char tmp[4096];
            int n = recv(fd, tmp, static_cast<int>(sizeof tmp), 0);
            if (n <= 0)
                return false;
            buf.append(tmp, static_cast<size_t>(n));
        }
        std::memcpy(dst, buf.data(), len);
        buf.erase(0, len);
        return true;
    }
};

bool send_all(socket_t fd, const std::string& s) {
    size_t sent = 0;
    while (sent < s.size()) {
        int n = send(fd, s.data() + sent,
                     static_cast<int>(s.size() - sent), 0);
        if (n <= 0)
            return false;
        sent += static_cast<size_t>(n);
    }
    return true;
}

// "REQ <rid> <len>" / "RESULT <id> OK|ERR <len>" 的头部行
std::string header_line(const std::string& kind, int id,
                        const std::string& status, size_t len) {
    std::string out = kind + " " + std::to_string(id);
    if (!status.empty())
        out += " " + status;
    out += " " + std::to_string(len) + "\n";
    return out;
}

std::string bytes_to_string(const std::vector<std::byte>& v) {
    std::string s;
    s.resize(v.size());
    std::memcpy(s.data(), v.data(), v.size());
    return s;
}

std::vector<std::byte> string_to_bytes(const std::string& s) {
    std::vector<std::byte> v(s.size());
    std::memcpy(v.data(), s.data(), s.size());
    return v;
}

// ------------------------------------------------------------------
// 连接读线程：外部行 → 按类型投递给 bridge actor（CALL）或
// external_echo 实现 actor（RESULT）。捕获 actor_addr（弱引用），
// 发送前升级——线程被进程退出强杀时不 unwind，强引用会保活 actor。
// ------------------------------------------------------------------
void conn_read_loop(socket_t fd, caf::actor_addr bridge_addr,
                    caf::actor_addr impl_addr,
                    std::shared_ptr<ActiveConn> active) {
    LineReader reader{fd, {}, active};
    std::string head;
    while (reader.read_line(head)) {
        // 头部：<KIND> <id> [<status>] <len>\n
        // 用最后一个空格切出 len，前面按空格分 token
        auto last_space = head.rfind(' ');
        if (last_space == std::string::npos)
            break;
        std::string len_str = head.substr(last_space + 1);
        while (!len_str.empty()
               && (len_str.back() == '\n' || len_str.back() == '\r'))
            len_str.pop_back();
        std::string prefix = head.substr(0, last_space);
        // prefix: "CALL <id> <svc>" 或 "RESULT <id> OK|ERR"
        auto sp1 = prefix.find(' ');
        if (sp1 == std::string::npos)
            break;
        std::string kind = prefix.substr(0, sp1);
        std::string rest = prefix.substr(sp1 + 1);
        auto sp2 = rest.find(' ');
        if (sp2 == std::string::npos)
            break;
        std::string id_str = rest.substr(0, sp2);
        std::string third = rest.substr(sp2 + 1);

        size_t len = 0;
        try {
            len = static_cast<size_t>(std::stoull(len_str));
        } catch (...) {
            break;
        }
        if (len > 64u * 1024u * 1024u) // 64MB 上限防恶意长度
            break;
        std::string payload;
        payload.resize(len);
        if (len > 0 && !reader.read_exact(payload.data(), len))
            break;
        // 跳过 payload 后的行终止符
        char tail = 0;
        if (!reader.read_exact(&tail, 1) || tail != '\n')
            break;

        int id = 0;
        try {
            id = std::stoi(id_str);
        } catch (...) {
            break;
        }

        // Python 主动调用 exe
        if (kind == "CALL") {
            // third = 服务名
            auto strong = caf::actor_cast<caf::actor>(bridge_addr);
            if (strong)
                caf::anon_send(strong, ext_call_atom_v, id, third,
                               std::move(payload));
        } else if (kind == "RESULT") {  // exe 调用 Python，异步回调
            // third = OK | ERR
            bool ok = (third == "OK");
            auto strong = caf::actor_cast<caf::actor>(impl_addr);
            if (strong)
                caf::anon_send(strong, ext_result_atom_v, id, ok,
                               std::move(payload));
        }
        // 其他行忽略
    }
    close_socket(fd);
}

// ------------------------------------------------------------------
// 连接写线程：消费 WriteQueue，串行写 socket
// ------------------------------------------------------------------
void conn_write_loop(socket_t fd, std::shared_ptr<WriteQueue> wq) {
    std::string line;
    while (wq->pop(line)) {
        if (!send_all(fd, line))
            break;
    }
    close_socket(fd);
}

// ------------------------------------------------------------------
// listener 线程：accept → 每连接一对读写线程；连接 wq 注册给 active
// ------------------------------------------------------------------
void listener_loop(socket_t listener, caf::actor_addr bridge_addr,
                   caf::actor_addr impl_addr,
                   std::shared_ptr<ActiveConn> active) {
    fprintf(stderr, "[Bridge] listener thread started, accepting...\n");
    fflush(stderr);
    while (true) {
        socket_t fd = accept(listener, nullptr, nullptr);
        if (fd == k_invalid_socket) {
            // 优雅关闭：shutdown_and_join() close listener 打断阻塞的
            // accept（WSAEINTR），预期行为，不打噪音；非关闭期的
            // accept 失败才打（端口耗尽等，调试有用）。
            if (!active->is_shutting_down()) {
#ifdef _WIN32
                fprintf(stderr, "[Bridge] accept failed, err=%d\n",
                        WSAGetLastError());
#else
                fprintf(stderr, "[Bridge] accept failed, errno=%d\n", errno);
#endif
                fflush(stderr);
            }
            break;
        }
        auto wq = std::make_shared<WriteQueue>();
        active->set_wq(wq); // 本连接成为"当前连接"（v1 单连接语义）
        active->add_conn(fd);
        active->add_thread(std::thread(conn_write_loop, fd, wq));
        active->add_thread(
            std::thread(conn_read_loop, fd, bridge_addr, impl_addr, active));
    }
    close_socket(listener);
}

} // namespace

caf::actor spawn_bridge_actor(caf::actor_system& sys, caf::actor registry,
                              std::uint16_t port,
                              const std::string& node_name) {
#ifdef _WIN32
    static std::atomic<bool> wsock_init{false};
    if (!wsock_init.exchange(true)) {
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
    }
#endif

    // ---- 活动连接：impl actor（REQ）与 bridge actor（RESULT）经它写
    //      socket（转发到当前连接的 wq，见 ActiveConn 注释）----
    auto active = std::make_shared<ActiveConn>();

    // ---- external_echo 实现 actor：集群调用（envelope）→ REQ 出站，
    //      外部 RESULT 到达后 deliver 给调用方 ----
    auto impl = sys.spawn(
        [active](caf::stateful_actor<BridgeImplState>* self) {
            return caf::behavior{
                // 发给 Python
                [self, active](plugin_envelope& env) -> caf::result<std::string> {
                    auto rid = ++self->state().seq;
                    std::string payload = bytes_to_string(env.payload);
                    auto rp = self->make_response_promise<std::string>();
                    self->state().pending.emplace(rid, rp);
                    active->push(header_line("REQ", rid, "", payload.size())
                                 + payload + "\n");
                    return rp;
                },
                [self](ext_result_atom, int rid, bool ok,
                       const std::string& payload) {
                    auto it = self->state().pending.find(rid);
                    if (it == self->state().pending.end()) {
                        LOG_WARN("[Bridge] stale external result for rid="
                                 + std::to_string(rid));
                        return;
                    }
                    if (ok) {
                        it->second.deliver(payload);
                    } else {
                        it->second.deliver(caf::make_error(
                            caf::sec::runtime_error, payload));
                    }
                    self->state().pending.erase(it);
                },
                [self](caf::exit_msg&) { self->quit(); }};
        });

    // 注册服务：registry 持有 impl（锚），proxy 自动生成并导出。
    // 必须【同步确认】注册完成再返回——main 的节点引导随后会查
    // exported_actors（只查一次，跨 sender 无顺序保证），若 register 还没
    // 被 registry 处理，manifest 上报不含 external_echo → master 路由
    // no_such_key。同 sender FIFO：register(send) 先于 list(request) 到达。
    caf::scoped_actor self{sys};
    self->send(registry, register_atom_v, std::string("external_echo"), impl,
               std::string("bridge"));
    bool registered = false;
    self->request(registry, std::chrono::seconds(2), list_services_atom_v)
        .receive(
            [&](std::vector<std::string>& names) {
                registered = std::find(names.begin(), names.end(),
                                       "external_echo")
                             != names.end();
            },
            [&](caf::error& e) {
                LOG_ERROR("[Bridge] list_services failed: "
                          + caf::to_string(e));
            });
    if (!registered) {
        LOG_ERROR("[Bridge] external_echo registration not confirmed");
    }

    auto impl_addr = caf::actor_cast<caf::actor_addr>(impl);

    // ---- bridge 主 actor：外部 CALL → resolve 本地服务 → 信封调用 ----
    // exit 时连 impl 一起杀：registry 关机只 send_exit proxy 不杀 impl，
    // 若 impl 无人 send_exit → actor_system 析构 join 它 → 进程挂起。
    auto bridge = sys.spawn(
        [registry, active, impl](caf::stateful_actor<BridgeState>* self) {
            return caf::behavior{
                // Python 发来的
                [self, registry, active](ext_call_atom, int id,
                                         const std::string& svc,
                                         const std::string& payload) {
                    auto respond = [active, id](bool ok,
                                                std::string body) {
                        active->push(header_line("RESULT", id,
                                                 ok ? "OK" : "ERR",
                                                 body.size())
                                     + body + "\n");
                    };
                    self->request(registry, std::chrono::seconds(2),
                                  resolve_atom_v, svc)
                        .then(
                            [self, active, svc, payload,
                             respond](caf::actor& proxy) {
                                if (!proxy) {
                                    respond(false,
                                            "service not found: " + svc);
                                    return;
                                }
                                plugin_envelope env;
                                env.sub_proto = k_bridge_sub_proto;
                                env.payload = string_to_bytes(payload);
                                self->request(proxy, std::chrono::seconds(5),
                                              std::move(env))
                                    .then(
                                        [respond](std::string& r) {
                                            respond(true, std::move(r));
                                        },
                                        [respond](caf::error& e) {
                                            respond(false,
                                                    caf::to_string(e));
                                        });
                            },
                            [respond](caf::error& e) {
                                respond(false, caf::to_string(e));
                            });
                },
                [self, impl, active](caf::exit_msg&) {
                    self->send_exit(impl, caf::exit_reason::user_shutdown);
                    // 优雅关闭 socket 线程并 join：detached 线程在进程
                    // 退出时被强杀不 unwind → 栈上 shared_ptr 泄漏
                    // （CRT dump 报 "REQ ..." 残留）。join 前 close 全部
                    // fd + wq->close()，阻塞点（accept/recv/pop）立即返回。
                    active->shutdown_and_join();
                    self->quit();
                }};
        });

    // ---- listener：bind/listen/accept ----
    socket_t listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener == k_invalid_socket) {
        LOG_ERROR("[Bridge] socket() failed, bridge disabled");
        return bridge; // actor 仍在，只是无连接
    }
    // SO_REUSEADDR：强杀/崩溃后 TIME_WAIT 残留端口，bind 立即复用。
    // 否则测试中 Stop-Process 强杀 → 立刻重跑脚本 → WSAEADDRINUSE
    // （实测：bind/listen on 127.0.0.1:48060 failed）。
    {
        int reuse = 1;
        setsockopt(listener, SOL_SOCKET, SO_REUSEADDR,
                   reinterpret_cast<const char*>(&reuse), sizeof(reuse));
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    // 绑 0.0.0.0：WSL 里的外部语言客户端经宿主 IP 连接（WSL2 的
    // 127.0.0.1 是 WSL 自己的 loopback，连不到 Windows 监听）。
    // 生产部署应改回 INADDR_LOOPBACK 并配合防火墙。
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port); // 必须设！遗漏 = bind 随机端口（踩过）
    if (bind(listener, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0
        || listen(listener, 8) != 0) {
        LOG_ERROR("[Bridge] bind/listen on 127.0.0.1:" + std::to_string(port)
                  + " failed");
        close_socket(listener);
        return bridge;
    }
    LOG_INFO("[Bridge] " + node_name + " listening on 127.0.0.1:"
             + std::to_string(port) + " (external_echo registered)");
    active->set_listener(listener);

    auto bridge_addr = caf::actor_cast<caf::actor_addr>(bridge);
    active->add_thread(
        std::thread(listener_loop, listener, bridge_addr, impl_addr, active));
    return bridge;
}

} // namespace caf_plugin_system
