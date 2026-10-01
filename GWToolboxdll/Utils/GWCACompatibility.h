#pragma once

#include <cstdint>
#include <Windows.h>

namespace GW {
    struct CharContext;
    namespace UI { enum class UIMessage : uint32_t; }
}

namespace GWCACompatibility {
    bool Initialize(HMODULE module);
    void Terminate();
    uint32_t& PlayerFlags(GW::CharContext& context);
    const wchar_t* PlayerEmail(const GW::CharContext& context);
    uint32_t NativePacketHeader(uint32_t header);
    uint32_t LegacyPacketCount(uint32_t native_count);
    GW::UI::UIMessage NativeUIMessage(GW::UI::UIMessage message);
}
