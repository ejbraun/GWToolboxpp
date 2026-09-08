#include <GWRL/Wire.h>
#include <Windows.h>
#include <bcrypt.h>
#include <array>
#include <stdexcept>

namespace Gwrl {
    bool JsonObject(const std::string_view json)
    {
        if (json.empty() || json.size() > MaximumPayload || json.find('\0') != json.npos
            || json.find_first_not_of(" \t\r\n") == json.npos
            || json[json.find_first_not_of(" \t\r\n")] != '{'
            || !MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, json.data(), static_cast<int>(json.size()), nullptr, 0)) return false;
        auto quoted = false, escaped = false;
        auto depth = 0;
        for (const auto c : json) {
            if (quoted) {
                if (escaped) escaped = false;
                else if (c == '\\') escaped = true;
                else if (c == '"') quoted = false;
            }
            else if (c == '"') quoted = true;
            else if (c == '{' || c == '[') { if (++depth > 32) return false; }
            else if (c == '}' || c == ']') { if (--depth < 0) return false; }
        }
        return !quoted && depth == 0 && !glz::validate_json(json);
    }

    std::string NewToken()
    {
        std::array<unsigned char, 16> bytes{};
        if (BCryptGenRandom(nullptr, bytes.data(), static_cast<ULONG>(bytes.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0)
            throw std::runtime_error("GWRL cannot create a registration identity");
        constexpr auto hex = "0123456789abcdef";
        auto result = std::string();
        for (const auto byte : bytes) { result += hex[byte >> 4]; result += hex[byte & 15]; }
        return result;
    }
}
