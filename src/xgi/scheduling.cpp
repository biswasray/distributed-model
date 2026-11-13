#include "xgi/scheduling.hpp"

namespace xgi {

namespace {
bool usable(const GroupLoad& g, double now, const std::string& exclude) {
    return g.healthy && g.suspect_until <= now && g.group_id != exclude;
}
}  // namespace

std::optional<size_t> select_group(const std::vector<GroupLoad>& groups, double now, const std::string& exclude) {
    std::optional<size_t> best;
    for (size_t i = 0; i < groups.size(); ++i) {
        const auto& g = groups[i];
        if (!usable(g, now, exclude) || g.slots < 1 || g.inflight >= g.slots) continue;
        if (!best) {
            best = i;
            continue;
        }
        const auto& b = groups[*best];
        // Compare inflight/slots without floating point: a.i * b.s vs b.i * a.s
        long long lhs = static_cast<long long>(g.inflight) * b.slots;
        long long rhs = static_cast<long long>(b.inflight) * g.slots;
        if (lhs < rhs || (lhs == rhs && g.idle_since < b.idle_since)) best = i;
    }
    return best;
}

bool any_routable(const std::vector<GroupLoad>& groups, double now, const std::string& exclude) {
    for (const auto& g : groups)
        if (usable(g, now, exclude)) return true;
    return false;
}

}  // namespace xgi
