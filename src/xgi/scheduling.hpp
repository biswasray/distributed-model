// Router group selection (architecture/04-router.md §3). Pure: no I/O, no locking.
#pragma once

#include <optional>
#include <string>
#include <vector>

namespace xgi {

struct GroupLoad {
    std::string group_id;
    int inflight = 0;
    int slots = 1;
    double idle_since = 0;       // monotonic time inflight last dropped to 0 (smaller = idle longer)
    double suspect_until = 0;    // monotonic; group is skipped until then
    bool healthy = false;
};

// Index of the group to dispatch to: healthy, not suspect, not `exclude`, inflight < slots,
// minimum inflight/slots; ties go to the group idle the longest. nullopt if all are busy.
std::optional<size_t> select_group(const std::vector<GroupLoad>& groups, double now, const std::string& exclude = "");

// True if at least one group can ever take traffic (healthy and not excluded), busy or not.
bool any_routable(const std::vector<GroupLoad>& groups, double now, const std::string& exclude = "");

}  // namespace xgi
