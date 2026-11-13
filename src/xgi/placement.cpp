#include "xgi/placement.hpp"

#include <algorithm>
#include <format>

namespace xgi {

Placement place_group(const PoolConfig& pool, const std::vector<FreeNode>& free_nodes, int spares_reserve,
                      const std::vector<PinnedEntry>& pinned, const std::set<std::string>& pool_model_files) {
    Placement out;
    const int need = pool.nodes_per_group;
    const int nfree = static_cast<int>(free_nodes.size());

    if (nfree - need < spares_reserve) {
        out.reason = nfree >= need
                         ? std::format("would consume spares ({} free, {} needed, {} spares)", nfree, need, spares_reserve)
                         : std::format("not enough free nodes ({} free, {} needed)", nfree, need);
        return out;
    }

    auto is_pinned = [&](const FreeNode& n) {
        return std::any_of(pinned.begin(), pinned.end(), [&](const PinnedEntry& p) {
            return p.pool == pool.name && p.role == "head" && (p.host == n.node_id || p.host == n.hostname);
        });
    };

    std::vector<const FreeNode*> heads;
    for (const auto& n : free_nodes)
        if (n.models.contains(pool.model_file)) heads.push_back(&n);
    if (heads.empty()) {
        out.reason = std::format("no free node has model file {}", pool.model_file);
        return out;
    }
    // Pinned-for-this-pool first, then most free RAM; node_id keeps it deterministic.
    std::sort(heads.begin(), heads.end(), [&](const FreeNode* a, const FreeNode* b) {
        bool pa = is_pinned(*a), pb = is_pinned(*b);
        if (pa != pb) return pa;
        if (a->ram_available != b->ram_available) return a->ram_available > b->ram_available;
        return a->node_id < b->node_id;
    });
    const FreeNode* head = heads.front();

    auto holds_any_model = [&](const FreeNode& n) {
        return std::any_of(n.models.begin(), n.models.end(), [&](const std::string& m) { return pool_model_files.contains(m); });
    };
    std::vector<const FreeNode*> workers;
    for (const auto& n : free_nodes)
        if (&n != head) workers.push_back(&n);
    // Nodes without model files first (keep model holders available as heads), then most free RAM.
    std::sort(workers.begin(), workers.end(), [&](const FreeNode* a, const FreeNode* b) {
        bool ha = holds_any_model(*a), hb = holds_any_model(*b);
        if (ha != hb) return !ha;
        if (a->ram_available != b->ram_available) return a->ram_available > b->ram_available;
        return a->node_id < b->node_id;
    });
    if (static_cast<int>(workers.size()) < need - 1) {
        out.reason = std::format("not enough workers ({} available, {} needed)", workers.size(), need - 1);
        return out;
    }

    out.ok = true;
    out.head = head->node_id;
    for (int i = 0; i < need - 1; ++i) out.workers.push_back(workers[static_cast<size_t>(i)]->node_id);
    return out;
}

}  // namespace xgi
