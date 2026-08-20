#pragma once
#include "common/message_tags.hpp"
#include <caf/all.hpp>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <vector>

// 消息标签集中定义于 common/message_tags.def（X-macro 唯一数据源）

// ------------------------------------------------------------------
// 改进后的 Checkpoint 文件格式（v1）
// ------------------------------------------------------------------
//   魔数(4) + 版本(2) + 名字长度(2) + 时间戳(8) + CRC32(4) + 数据长度(8)
//   + 名字(N) + 数据(M)
//
//   总头部长度：28 字节（固定）
// ------------------------------------------------------------------
struct CheckpointHeader {
    char     magic[4]      = {'C', 'K', 'P', 'T'};  // 魔数
    uint16_t version       = 1;                       // 格式版本
    uint16_t name_len      = 0;                       // 插件名实际长度
    int64_t  timestamp     = 0;                       // 保存时间戳
    uint32_t crc32         = 0;                       // 标准 CRC32（查表法）
    uint64_t data_len      = 0;                       // 数据长度
};

class CheckpointManager : public caf::event_based_actor {
public:
    CheckpointManager(caf::actor_config& cfg, std::filesystem::path dir);

    caf::behavior make_behavior() override;

private:
    std::filesystem::path checkpoint_dir_;

    // 标准 CRC32（IEEE 802.3）
    static uint32_t compute_crc32(const std::vector<std::byte>& data);

    // 小端序读写辅助（保证跨平台）
    template<typename T>
    static void write_le(std::ostream& os, T value);
    template<typename T>
    static bool read_le(std::istream& is, T& value);
};
