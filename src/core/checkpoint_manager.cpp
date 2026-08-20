#include "checkpoint_manager.hpp"
#include <iostream>
#include <cstring>
#include <array>

// ------------------------------------------------------------------
// 标准 CRC32 查表实现（IEEE 802.3）
// ------------------------------------------------------------------
static const std::array<uint32_t, 256> crc32_table = [] {
    std::array<uint32_t, 256> table{};
    for (uint32_t i = 0; i < 256; ++i) {
        uint32_t crc = i;
        for (int j = 0; j < 8; ++j) {
            crc = (crc >> 1) ^ (0xEDB88320 & -(crc & 1));
        }
        table[i] = crc;
    }
    return table;
}();

uint32_t CheckpointManager::compute_crc32(const std::vector<std::byte>& data) {
    uint32_t crc = 0xFFFFFFFF;
    for (auto b : data) {
        crc = crc32_table[(crc ^ static_cast<uint8_t>(b)) & 0xFF] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFF;
}

// ------------------------------------------------------------------
// 小端序读写（显式处理，不依赖平台字节序）
// ------------------------------------------------------------------
template<typename T>
void CheckpointManager::write_le(std::ostream& os, T value) {
    for (size_t i = 0; i < sizeof(T); ++i) {
        os.put(static_cast<char>((value >> (i * 8)) & 0xFF));
    }
}

template<typename T>
bool CheckpointManager::read_le(std::istream& is, T& value) {
    value = 0;
    for (size_t i = 0; i < sizeof(T); ++i) {
        char c;
        if (!is.get(c)) return false;
        value |= (static_cast<T>(static_cast<unsigned char>(c)) << (i * 8));
    }
    return true;
}

// 显式实例化（避免链接问题）
template void CheckpointManager::write_le<uint16_t>(std::ostream&, uint16_t);
template void CheckpointManager::write_le<int64_t>(std::ostream&, int64_t);
template void CheckpointManager::write_le<uint32_t>(std::ostream&, uint32_t);
template void CheckpointManager::write_le<uint64_t>(std::ostream&, uint64_t);
template bool CheckpointManager::read_le<uint16_t>(std::istream&, uint16_t&);
template bool CheckpointManager::read_le<int64_t>(std::istream&, int64_t&);
template bool CheckpointManager::read_le<uint32_t>(std::istream&, uint32_t&);
template bool CheckpointManager::read_le<uint64_t>(std::istream&, uint64_t&);

// ------------------------------------------------------------------

CheckpointManager::CheckpointManager(std::filesystem::path dir)
    : checkpoint_dir_(std::move(dir)) {
    std::filesystem::create_directories(checkpoint_dir_);
}

auto CheckpointManager::make_behavior(caf::event_based_actor* self) {
    return caf::behavior{
        [=](save_state_atom, const std::string& plugin_name,
            const std::vector<std::byte>& data) -> bool {
            try {
                auto path = checkpoint_dir_ / (plugin_name + ".ckpt");
                auto tmp_path = path.string() + ".tmp";

                CheckpointHeader hdr{};
                hdr.name_len  = static_cast<uint16_t>(plugin_name.size());
                hdr.timestamp = std::chrono::system_clock::now().time_since_epoch().count();
                hdr.crc32     = compute_crc32(data);
                hdr.data_len  = data.size();

                // 先写入临时文件，再原子 rename（防止写一半崩溃）
                {
                    std::ofstream ofs(tmp_path, std::ios::binary);
                    if (!ofs) return false;

                    ofs.write(hdr.magic, 4);
                    write_le(ofs, hdr.version);
                    write_le(ofs, hdr.name_len);
                    write_le(ofs, hdr.timestamp);
                    write_le(ofs, hdr.crc32);
                    write_le(ofs, hdr.data_len);
                    ofs.write(plugin_name.c_str(), hdr.name_len);
                    ofs.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));

                    ofs.flush();
                    if (!ofs) return false;
                }

                std::filesystem::rename(tmp_path, path);
                return true;
            } catch (...) {
                return false;
            }
        },

        [=](restore_state_atom, const std::string& plugin_name) -> std::vector<std::byte> {
            auto path = checkpoint_dir_ / (plugin_name + ".ckpt");
            if (!std::filesystem::exists(path)) return {};

            std::ifstream ifs(path, std::ios::binary);
            if (!ifs) return {};

            // 读取魔数
            char magic[4];
            ifs.read(magic, 4);
            if (ifs.gcount() != 4 || std::memcmp(magic, "CKPT", 4) != 0) {
                std::cerr << "[Checkpoint] Bad magic for " << plugin_name << std::endl;
                return {};
            }

            CheckpointHeader hdr;
            std::memcpy(hdr.magic, magic, 4);
            if (!read_le(ifs, hdr.version)) return {};
            if (!read_le(ifs, hdr.name_len)) return {};
            if (!read_le(ifs, hdr.timestamp)) return {};
            if (!read_le(ifs, hdr.crc32)) return {};
            if (!read_le(ifs, hdr.data_len)) return {};

            if (hdr.version != 1) {
                std::cerr << "[Checkpoint] Unsupported version " << hdr.version
                          << " for " << plugin_name << std::endl;
                return {};
            }

            std::string name(hdr.name_len, '\0');
            ifs.read(name.data(), hdr.name_len);

            std::vector<std::byte> data(hdr.data_len);
            ifs.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(hdr.data_len));

            if (static_cast<uint64_t>(ifs.gcount()) != hdr.data_len) {
                std::cerr << "[Checkpoint] Truncated file for " << plugin_name << std::endl;
                return {};
            }

            if (compute_crc32(data) != hdr.crc32) {
                std::cerr << "[Checkpoint] CRC mismatch for " << plugin_name << std::endl;
                return {};
            }
            return data;
        }
    };
}
