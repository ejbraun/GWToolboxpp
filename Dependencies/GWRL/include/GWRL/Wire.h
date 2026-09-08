#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include <glaze/glaze.hpp>

namespace Gwrl {
    inline constexpr uint32_t Magic = 0x4c525747;
    inline constexpr uint16_t Major = 1;
    inline constexpr uint16_t Minor = 0;
    inline constexpr uint32_t MaximumPayload = 65536;
    inline constexpr uint32_t MaximumQueue = 64;
    inline constexpr uint32_t MaximumModuleQueue = 48;
    inline constexpr uint32_t MaximumRouteQueue = 8;
    inline constexpr uint32_t MaximumRoutes = 32;
    inline constexpr uint32_t MaximumVersions = 8;
    inline constexpr std::string_view RoutingCapability = "module_routing_v1";
    inline constexpr auto ReadOptions = glz::opts{.error_on_unknown_keys = false};
    inline constexpr auto EnvelopeOptions = glz::opts{.error_on_unknown_keys = false, .error_on_missing_keys = true};

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

    struct Envelope {
        uint32_t major = Major;
        uint32_t minor = Minor;
        std::string client;
        std::string session_id;
        uint32_t pid = 0;
        std::string process_started;
        std::string type;
        std::string request_id;
        std::optional<std::string> recipient;
        std::optional<std::string> kind;
        std::optional<uint32_t> route_version;
        std::optional<std::string> route_session;
        std::optional<glz::raw_json> payload;
        std::optional<glz::raw_json> routing;
        std::optional<std::string> code;
        std::optional<std::string> detail;
    };

    struct RouteDescriptor {
        std::string recipient;
        std::string registration_id;
        std::vector<uint32_t> versions;
    };
    struct RouteOffer {
        std::string revision;
        std::vector<RouteDescriptor> routes;
    };
    struct RouteSelection {
        std::string recipient;
        std::string registration_id;
        std::string peer_registration_id;
        uint32_t route_version = 0;
        std::string route_session;
        bool operator==(const RouteSelection&) const = default;
    };
    struct RouteSelections {
        std::string revision;
        std::vector<RouteSelection> routes;
    };
    struct RouteAck {
        std::string revision;
        bool accepted = false;
        std::string code;
        std::vector<RouteSelection> routes;
    };

    inline bool IsIdentifier(const std::string_view value, const size_t maximum = 64)
    {
        if (value.empty() || value.size() > maximum) return false;
        for (const auto c : value) {
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z')
                || (c >= 'A' && c <= 'Z') || c == '-' || c == '_')) return false;
        }
        return true;
    }
    inline bool IsRecipient(const std::string_view value)
    {
        if (!IsIdentifier(value)) return false;
        for (const auto c : value) if (c >= 'A' && c <= 'Z') return false;
        return true;
    }
    inline bool IsToken(const std::string_view value)
    {
        if (value.size() != 32) return false;
        for (const auto c : value) if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
        return true;
    }
    bool JsonObject(const std::string_view json);
    std::string NewToken();
}
