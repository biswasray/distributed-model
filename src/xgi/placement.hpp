// Placement algorithm (architecture/03-coordinator.md §4). Pure: no I/O, no locking.
#pragma once

#include <cstdint>
#include <set>
#include <string>
#include <vector>

#include "common/config.hpp"

namespace xgi {

struct FreeNode {
    std::string node_id;
    std::string hostname;
    std::uint64_t ram_available = 0;
    std::set<std::string> models;  // normalised relative paths
};

struct Placement {
    bool ok = false;
    std::string head;
    std::vector<std::string> workers;  // pipeline order
    std::string reason;                // why it failed
};

// Places one group of `pool` on `free_nodes` (alive, not draining, unassigned).
// `spares_reserve` nodes must stay free afterwards (0 during repair).
// `pool_model_files` = model_file of every pool, used to keep model holders available as heads.
Placement place_group(const PoolConfig& pool, const std::vector<FreeNode>& free_nodes, int spares_reserve,
                      const std::vector<PinnedEntry>& pinned, const std::set<std::string>& pool_model_files);

}  // namespace xgi
