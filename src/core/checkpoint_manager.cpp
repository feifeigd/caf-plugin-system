#include "checkpoint_manager.hpp"
#include <iostream>
#include <cstring>

struct Checkpoint {
    std::string plugin_name;
    std::vector<uint8_t> data;
    int64_t timestamp;
    uint32_t crc32;
};

CheckpointManager::CheckpointManager(std::filesystem::path dir)
    : checkpoint_dir_(std::move(dir)) {
    std::filesystem::create_directories(checkpoint_dir_);
}

uint32_t CheckpointManager::compute_crc32(const std::vector<uint8_t>& data) {
    uint32_t crc = 0;
    for (auto b : data) crc = crc * 31 + b;
    return crc;
}

auto CheckpointManager::make_behavior(caf::event_based_actor* self) {
    return caf::behavior{
        [=](save_state_atom, const std::string& plugin_name,
            const std::vector<uint8_t>& data) -> bool {
            try {
                Checkpoint cp{plugin_name, data,
                    std::chrono::system_clock::now().time_since_epoch().count(),
                    compute_crc32(data)};

                auto path = checkpoint_dir_ / (plugin_name + ".ckpt");
                std::ofstream ofs(path, std::ios::binary);
                if (!ofs) return false;

                char name_buf[64] = {};
                std::strncpy(name_buf, cp.plugin_name.c_str(), 63);
                ofs.write(name_buf, 64);
                ofs.write(reinterpret_cast<const char*>(&cp.timestamp), 8);
                ofs.write(reinterpret_cast<const char*>(&cp.crc32), 4);
                size_t len = cp.data.size();
                ofs.write(reinterpret_cast<const char*>(&len), 8);
                ofs.write(reinterpret_cast<const char*>(cp.data.data()), static_cast<std::streamsize>(cp.data.size()));
                ofs.flush();
                return true;
            } catch (...) {
                return false;
            }
        },

        [=](restore_state_atom, const std::string& plugin_name) -> std::vector<uint8_t> {
            auto path = checkpoint_dir_ / (plugin_name + ".ckpt");
            if (!std::filesystem::exists(path)) return {};

            std::ifstream ifs(path, std::ios::binary);
            if (!ifs) return {};

            char name_buf[64] = {};
            ifs.read(name_buf, 64);
            int64_t timestamp;
            ifs.read(reinterpret_cast<char*>(&timestamp), 8);
            uint32_t crc;
            ifs.read(reinterpret_cast<char*>(&crc), 4);
            size_t len;
            ifs.read(reinterpret_cast<char*>(&len), 8);

            std::vector<uint8_t> data(len);
            ifs.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(len));

            if (compute_crc32(data) != crc) {
                std::cout << "[Checkpoint] CRC mismatch for " << plugin_name << std::endl;
                return {};
            }
            return data;
        }
    };
}
