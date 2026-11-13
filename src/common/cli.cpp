#include "common/cli.hpp"

#include <stdexcept>

namespace xgi {

CliArgs::CliArgs(int argc, char** argv, std::set<std::string> switches) {
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a.rfind("--", 0) == 0 || (a.size() == 2 && a[0] == '-')) {
            auto eq = a.find('=');
            if (eq != std::string::npos) {
                values_[a.substr(0, eq)] = a.substr(eq + 1);
            } else if (switches.contains(a) || i + 1 >= argc) {
                values_[a] = "";
            } else {
                values_[a] = argv[++i];
            }
        } else {
            positional_.push_back(a);
        }
    }
}

bool CliArgs::has(const std::string& flag) const { return values_.contains(flag); }

std::optional<std::string> CliArgs::get(const std::string& flag) const {
    auto it = values_.find(flag);
    if (it == values_.end()) return std::nullopt;
    return it->second;
}

std::string CliArgs::get_or(const std::string& flag, const std::string& def) const {
    auto v = get(flag);
    return v ? *v : def;
}

int CliArgs::get_int(const std::string& flag, int def) const {
    auto v = get(flag);
    if (!v) return def;
    try {
        return std::stoi(*v);
    } catch (...) {
        throw std::invalid_argument(flag + " expects an integer, got '" + *v + "'");
    }
}

double CliArgs::get_double(const std::string& flag, double def) const {
    auto v = get(flag);
    if (!v) return def;
    try {
        return std::stod(*v);
    } catch (...) {
        throw std::invalid_argument(flag + " expects a number, got '" + *v + "'");
    }
}

std::vector<std::string> CliArgs::unknown(const std::set<std::string>& known) const {
    std::vector<std::string> out;
    for (auto& [k, _] : values_)
        if (!known.contains(k)) out.push_back(k);
    return out;
}

}  // namespace xgi
