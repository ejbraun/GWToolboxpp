#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace ObjectiveTimerHistory {
    struct Objective {
        std::string name;
        uint32_t status = 0;
        uint32_t start = 0;
        uint32_t done = 0;
        std::optional<uint32_t> indent;
        std::optional<uint32_t> duration;
    };

    struct Run {
        std::string name;
        uint32_t instance_start = 0;
        uint32_t utc_start = 0;
        std::vector<Objective> objectives;
        std::optional<uint32_t> duration;
        std::optional<std::string> run_id;
        std::optional<std::string> character_name;
    };

    struct Result {
        std::vector<Run> runs;
        std::vector<std::string> errors;
    };

    std::string Identity(const Run& run);
    Result Load(const std::filesystem::path& folder, size_t limit = 200);
    Result Save(const std::filesystem::path& folder, const std::vector<Run>& snapshot);
}
