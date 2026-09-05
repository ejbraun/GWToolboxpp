#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace Gwrl {
    inline constexpr uint32_t Magic = 0x4c525747;
    inline constexpr uint16_t Major = 1;
    inline constexpr uint16_t Minor = 0;
    inline constexpr uint32_t MaximumPayload = 65536;
    inline constexpr uint32_t MaximumQueue = 64;

    struct Bootstrap {
        uint32_t magic = Magic;
        uint16_t major = Major;
        uint16_t minor = Minor;
        uint32_t size = sizeof(Bootstrap);
        uint32_t published = 0;
        uint32_t controller_pid = 0;
        uint32_t target_pid = 0;
        uint64_t controller_started = 0;
        uint64_t target_started = 0;
        char session_id[33]{};
        char transaction_id[65]{};
        uint16_t reserved = 0;
        uint32_t hold_plugins = 0;
        wchar_t pipe_name[192]{};
    };
    static_assert(sizeof(Bootstrap) == 528);

    struct Artifact {
        std::string name;
        uint32_t version = 0;
        uint32_t abi = 0;
        std::string sha256;
        std::string path;
        std::string state;
        bool enabled = false;
        bool operator==(const Artifact&) const = default;
    };

    struct Message {
        uint32_t major = Major;
        uint32_t minor = Minor;
        std::string type;
        std::string session_id;
        std::string request_id;
        std::string transaction_id;
        std::string client = "toolbox";
        uint32_t pid = 0;
        std::string process_started;
        bool user_initiated = false;
        std::string state;
        std::string detail;
        std::string code;
        std::vector<std::string> capabilities;
        std::vector<Artifact> artifacts;
    };

    inline bool IsIdentifier(const std::string& value, const size_t maximum = 64)
    {
        if (value.empty() || value.size() > maximum) return false;
        for (const auto c : value) {
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z')
                || (c >= 'A' && c <= 'Z') || c == '-' || c == '_')) return false;
        }
        return true;
    }

    inline bool IsSha256(const std::string& value)
    {
        if (value.size() != 64) return false;
        for (const auto c : value) {
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
        }
        return true;
    }

    inline bool ValidateEnvelope(const Message& message, const std::string& session, const uint32_t pid, const uint64_t started)
    {
        return message.major == Major && message.client == "toolbox" && message.session_id == session
            && message.pid == pid && message.process_started == std::to_string(started)
            && IsIdentifier(message.request_id) && IsIdentifier(message.type);
    }

    inline bool IsManagedPlugin(const std::string& name)
    {
        return name == "DBBox.dll" || name == "SCTracker.dll";
    }

    inline bool ValidatePlan(const std::vector<Artifact>& artifacts)
    {
        if (artifacts.empty() || artifacts.size() > 3) return false;
        std::vector<std::string> names;
        for (const auto& a : artifacts) {
            if ((!IsManagedPlugin(a.name) && a.name != "GWToolboxdll.dll")
                || !a.version || !IsSha256(a.sha256)) return false;
            for (const auto& name : names) if (name == a.name) return false;
            names.push_back(a.name);
        }
        return true;
    }
}
