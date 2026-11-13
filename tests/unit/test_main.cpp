#include <string_view>

#include "testing.hpp"

int main(int argc, char** argv) {
    std::string_view filter = argc > 1 ? argv[1] : "";
    int ran = 0, failed_cases = 0;
    for (auto& c : testing::registry()) {
        if (!filter.empty() && std::string_view(c.name).find(filter) == std::string_view::npos) continue;
        int before = testing::failures();
        try {
            c.fn();
        } catch (const testing::RequireFailed&) {
        } catch (const std::exception& e) {
            testing::report(__FILE__, __LINE__, std::string("unexpected exception: ") + e.what());
        }
        ++ran;
        bool ok = testing::failures() == before;
        if (!ok) ++failed_cases;
        std::cout << (ok ? "[ OK ] " : "[FAIL] ") << c.name << "\n";
    }
    std::cout << std::format("\n{} test case(s), {} failed, {} check failure(s)\n", ran, failed_cases, testing::failures());
    return failed_cases == 0 ? 0 : 1;
}
