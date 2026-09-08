#pragma once

#include <functional>
#include <string>
#include <string_view>

namespace GW::Constants { enum class Language; }

namespace AsyncStringDecoder {
    using Completion = std::function<void(const wchar_t*)>;

    void Decode(std::wstring_view encoded, Completion completion,
        GW::Constants::Language language = static_cast<GW::Constants::Language>(0xff));
    [[nodiscard]] size_t PendingCount();
}
