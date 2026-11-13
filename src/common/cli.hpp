// Minimal "--flag value" command-line parser.
#pragma once

#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace xgi {

class CliArgs {
public:
    // `switches` are flags that take no value (e.g. "--help").
    CliArgs(int argc, char** argv, std::set<std::string> switches = {"--help", "-h", "--version"});

    bool has(const std::string& flag) const;
    std::optional<std::string> get(const std::string& flag) const;
    std::string get_or(const std::string& flag, const std::string& def) const;
    int get_int(const std::string& flag, int def) const;
    double get_double(const std::string& flag, double def) const;
    const std::vector<std::string>& positional() const { return positional_; }

    // Flags that were given but are not in `known`.
    std::vector<std::string> unknown(const std::set<std::string>& known) const;

private:
    std::map<std::string, std::string> values_;
    std::vector<std::string> positional_;
};

}  // namespace xgi
