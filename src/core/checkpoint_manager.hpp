#pragma once
#include <caf/all.hpp>
#include <filesystem>
#include <fstream>
#include <vector>

using save_state_atom = caf::atom_constant<caf::atom("savest")>;
using restore_state_atom = caf::atom_constant<caf::atom("restore")>;

class CheckpointManager {
public:
    explicit CheckpointManager(std::filesystem::path dir = "./checkpoints");
    auto make_behavior(caf::event_based_actor* self);

private:
    std::filesystem::path checkpoint_dir_;
    static uint32_t compute_crc32(const std::vector<uint8_t>& data);
};
