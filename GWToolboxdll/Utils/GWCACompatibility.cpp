#include "stdafx.h"

#include <array>
#include <cstring>
#include <functional>
#include <utility>
#include <MinHook.h>
#include <GWCA/Context/CharContext.h>
#include <GWCA/Managers/StoCMgr.h>
#include <GWCA/Managers/UIMgr.h>
#include <GWCA/Packets/StoC.h>
#include <GWCA/Utilities/Hooker.h>
#include <Logger.h>
#include <Utils/GWCACompatibility.h>

namespace {
    constexpr auto inserted_header = 0x194u;
    constexpr auto legacy_header_count = 0x1e7u;
    HMODULE patched_module = nullptr;
    bool active = false;
    thread_local bool registering_native_header = false;
    thread_local bool registering_native_message = false;

    using RegisterCallback = bool(__cdecl*)(GW::HookEntry*, uint32_t, const GW::StoC::PacketCallback&, int);
    using RemoveCallback = size_t(__cdecl*)(uint32_t, GW::HookEntry*);
    using EmulatePacket = bool(__cdecl*)(GW::Packet::StoC::PacketBase*);
    RegisterCallback register_callback_ret = nullptr;
    RemoveCallback remove_callback_ret = nullptr;
    EmulatePacket emulate_packet_ret = nullptr;
    decltype(&GW::UI::RegisterUIMessageCallback) register_ui_ret = nullptr;
    decltype(&GW::UI::RemoveUIMessageCallback) remove_ui_ret = nullptr;
    decltype(&GW::UI::RegisterFrameUIMessageCallback) register_frame_ret = nullptr;
    decltype(&GW::UI::SendUIMessage) send_ui_ret = nullptr;
    decltype(&GW::UI::SendFrameUIMessage) send_frame_ret = nullptr;
    void* native_ui_ret = nullptr;
    void* native_frame_ret = nullptr;
    std::array<void*, 10> hook_targets{};
    std::array<bool, 10> hook_created{};

    template <typename T>
    struct ScopedValue {
        T& target;
        const T previous;
        ScopedValue(T& value, const T replacement) : target(value), previous(std::exchange(value, replacement)) {}
        ~ScopedValue() { target = previous; }
    };

    bool OnRegisterCallback(GW::HookEntry* entry, const uint32_t header, const GW::StoC::PacketCallback& callback, const int altitude)
    {
        if (header >= legacy_header_count) return false;
        if (header < inserted_header) return register_callback_ret(entry, header, callback, altitude);
        const GW::StoC::PacketCallback translated = [callback, header](GW::HookStatus* status, GW::Packet::StoC::PacketBase* packet) {
            const ScopedValue legacy_header(packet->header, header);
            callback(status, packet);
        };
        // RegisterPacketCallback calls RemoveCallback internally with the already translated index.
        const ScopedValue native_registration(registering_native_header, true);
        return register_callback_ret(entry, header + 1, translated, altitude);
    }

    size_t OnRemoveCallback(const uint32_t header, GW::HookEntry* entry)
    {
        if (registering_native_header) return remove_callback_ret(header, entry);
        if (header >= legacy_header_count) return 0;
        return remove_callback_ret(header + (header >= inserted_header), entry);
    }

    bool OnEmulatePacket(GW::Packet::StoC::PacketBase* packet)
    {
        if (!packet || packet->header >= legacy_header_count) return false;
        // The new trailing InstanceLoadInfo flag cannot be reconstructed from a legacy packet.
        if (packet->header == GAME_SMSG_INSTANCE_LOAD_INFO) {
            Log::Log("[GWCACompatibility] InstanceLoadInfo emulation requires the updated packet layout");
            return false;
        }
        const ScopedValue native_header(packet->header, packet->header + (packet->header >= inserted_header));
        return emulate_packet_ret(packet);
    }

    void OnRegisterUI(GW::HookEntry* entry, const GW::UI::UIMessage message, const GW::UI::UIMessageCallback& callback, const int altitude)
    {
        const auto native = GWCACompatibility::NativeUIMessage(message);
        const GW::UI::UIMessageCallback translated = [callback, message](GW::HookStatus* status, GW::UI::UIMessage, void* wparam, void* lparam) {
            callback(status, message, wparam, lparam);
        };
        const ScopedValue native_registration(registering_native_message, true);
        register_ui_ret(entry, native, translated, altitude);
    }

    void OnRemoveUI(GW::HookEntry* entry, const GW::UI::UIMessage message)
    {
        remove_ui_ret(entry, registering_native_message ? message : GWCACompatibility::NativeUIMessage(message));
    }

    void OnRegisterFrame(GW::HookEntry* entry, const GW::UI::UIMessage message, const GW::UI::FrameUIMessageCallback& callback, const int altitude)
    {
        const GW::UI::FrameUIMessageCallback translated = [callback, message](GW::HookStatus* status, const GW::UI::Frame* frame, GW::UI::UIMessage, void* wparam, void* lparam) {
            callback(status, frame, message, wparam, lparam);
        };
        register_frame_ret(entry, GWCACompatibility::NativeUIMessage(message), translated, altitude);
    }

    bool OnSendUI(const GW::UI::UIMessage message, void* wparam, void* lparam)
    {
        return send_ui_ret(GWCACompatibility::NativeUIMessage(message), wparam, lparam);
    }

    bool OnSendFrame(GW::UI::Frame* frame, const GW::UI::UIMessage message, void* wparam, void* lparam)
    {
        return send_frame_ret(frame, GWCACompatibility::NativeUIMessage(message), wparam, lparam);
    }

    // Native dispatch already carries the new IDs; bypass the translation used by GWCA callers.
    void OnNativeUI(const GW::UI::UIMessage message, void* wparam, void* lparam)
    {
        GW::Hook::EnterHook();
        send_ui_ret(message, wparam, lparam);
        GW::Hook::LeaveHook();
    }

    void __fastcall OnNativeFrame(void* context, void*, const GW::UI::UIMessage message, void* wparam, void* lparam)
    {
        GW::Hook::EnterHook();
        send_frame_ret(reinterpret_cast<GW::UI::Frame*>(static_cast<BYTE*>(context) - 0xa8), message, wparam, lparam);
        GW::Hook::LeaveHook();
    }

    struct OperandPatch {
        const char* name;
        uintptr_t rva;
        uint32_t before;
        uint32_t after;
        size_t size = sizeof(uint32_t);
        DWORD old_protection = 0;
    };
    std::array operand_patches{
        OperandPatch{"GetMapID", 0xe3da, 0x234, 0x238},
        OperandPatch{"GetLanguage", 0xe441, 0x22c, 0x230},
        OperandPatch{"GetIsObserving: current map", 0xe458, 0x234, 0x238},
        OperandPatch{"GetIsObserving: observed map", 0xe45e, 0x230, 0x234},
        OperandPatch{"GetDistrict", 0xe471, 0x228, 0x22c},
        OperandPatch{"GetInstanceType", 0xe4f5, 0x23c, 0x240},
        OperandPatch{"GetPlayerNumber", 0x10905, 0x2ac, 0x2b0},
        OperandPatch{"StoC handler count", 0x14315, 0x1e7, 0x1e8},
        OperandPatch{"ItemFormula stride", 0xd868, 0x14, 0x18, 1}
    };

    bool WriteOperands(const bool enable)
    {
        auto* bytes = reinterpret_cast<BYTE*>(patched_module);
        auto success = true;
        for (auto& patch : operand_patches) {
            if (!VirtualProtect(bytes + patch.rva, patch.size, PAGE_EXECUTE_READWRITE, &patch.old_protection)) {
                success = false;
                break;
            }
        }
        if (success) {
            for (const auto& patch : operand_patches) {
                const auto value = enable ? patch.after : patch.before;
                std::memcpy(bytes + patch.rva, &value, patch.size);
            }
        }
        for (auto patch = operand_patches.rbegin(); patch != operand_patches.rend(); ++patch) {
            if (!patch->old_protection) continue;
            DWORD ignored = 0;
            if (!VirtualProtect(bytes + patch->rva, patch->size, patch->old_protection, &ignored)) success = false;
            patch->old_protection = 0;
        }
        if (!FlushInstructionCache(GetCurrentProcess(), bytes, 0x15000)) success = false;
        return success;
    }
}

bool GWCACompatibility::Initialize(const HMODULE module)
{
    if (active) return patched_module == module;
    if (!module || patched_module) return false;
    const auto* bytes = reinterpret_cast<const BYTE*>(module);
    for (const auto& patch : operand_patches) {
        if (std::memcmp(bytes + patch.rva, &patch.before, patch.size) != 0) {
            Log::Log("[GWCACompatibility] %s operand validation failed at RVA 0x%zx", patch.name, patch.rva);
            return false;
        }
    }
    // Translating an already populated registry would leave existing callbacks on the wrong packets.
    if (*reinterpret_cast<const uintptr_t*>(bytes + 0x53ebc) || *reinterpret_cast<const uintptr_t*>(bytes + 0x53ec0)) {
        Log::Log("[GWCACompatibility] GWCA packet registry is already initialized");
        return false;
    }
    const auto initialized = MH_Initialize();
    if (initialized != MH_OK && initialized != MH_ERROR_ALREADY_INITIALIZED) return false;
    patched_module = module;
    auto* writable_bytes = reinterpret_cast<BYTE*>(module);
    hook_targets = {writable_bytes + 0x14448, writable_bytes + 0x1455b, writable_bytes + 0x1462f,
        writable_bytes + 0x17a26, writable_bytes + 0x17b5a, writable_bytes + 0x17893,
        writable_bytes + 0x1694c, writable_bytes + 0x167c0, writable_bytes + 0x14e49, writable_bytes + 0x14ee4};
    const std::array<void*, 10> detours{
        reinterpret_cast<void*>(OnRegisterCallback), reinterpret_cast<void*>(OnRemoveCallback), reinterpret_cast<void*>(OnEmulatePacket),
        reinterpret_cast<void*>(OnRegisterUI), reinterpret_cast<void*>(OnRemoveUI), reinterpret_cast<void*>(OnRegisterFrame),
        reinterpret_cast<void*>(OnSendUI), reinterpret_cast<void*>(OnSendFrame), reinterpret_cast<void*>(OnNativeUI), reinterpret_cast<void*>(OnNativeFrame)};
    const std::array<void**, 10> originals{
        reinterpret_cast<void**>(&register_callback_ret), reinterpret_cast<void**>(&remove_callback_ret), reinterpret_cast<void**>(&emulate_packet_ret),
        reinterpret_cast<void**>(&register_ui_ret), reinterpret_cast<void**>(&remove_ui_ret), reinterpret_cast<void**>(&register_frame_ret),
        reinterpret_cast<void**>(&send_ui_ret), reinterpret_cast<void**>(&send_frame_ret), &native_ui_ret, &native_frame_ret};
    for (size_t i = 0; i < hook_targets.size(); ++i) {
        const auto result = MH_CreateHook(hook_targets[i], detours[i], originals[i]);
        if (result != MH_OK) {
            Log::Log("[GWCACompatibility] creating compatibility hook %zu failed: %d", i, result);
            Terminate();
            return false;
        }
        hook_created[i] = true;
    }
    if (!WriteOperands(true)) {
        Log::Log("[GWCACompatibility] writing compatibility operands failed");
        Terminate();
        return false;
    }
    for (const auto target : hook_targets) {
        const auto result = MH_EnableHook(target);
        if (result != MH_OK) {
            Log::Log("[GWCACompatibility] enabling compatibility hook failed: %d", result);
            Terminate();
            return false;
        }
    }
    active = true;
    Log::Log("[GWCACompatibility] enabled September 30 packet, UI-message, character-context and item-formula compatibility");
    return true;
}

void GWCACompatibility::Terminate()
{
    if (!patched_module) return;
    for (size_t i = 0; i < hook_targets.size(); ++i) {
        if (!hook_created[i]) continue;
        MH_DisableHook(hook_targets[i]);
        MH_RemoveHook(hook_targets[i]);
        hook_created[i] = false;
    }
    if (!WriteOperands(false)) Log::Log("[GWCACompatibility] restoring compatibility operands failed");
    hook_targets.fill(nullptr);
    patched_module = nullptr;
    active = false;
}

uint32_t& GWCACompatibility::PlayerFlags(GW::CharContext& context)
{
    if (!active) return context.player_flags;
    return *reinterpret_cast<uint32_t*>(reinterpret_cast<BYTE*>(&context) + 0x2ac);
}

const wchar_t* GWCACompatibility::PlayerEmail(const GW::CharContext& context)
{
    if (!active) return context.player_email;
    return reinterpret_cast<const wchar_t*>(reinterpret_cast<const BYTE*>(&context) + 0x3d0);
}

uint32_t GWCACompatibility::NativePacketHeader(const uint32_t header)
{
    return header + (active && header >= inserted_header && header < legacy_header_count);
}

uint32_t GWCACompatibility::LegacyPacketCount(const uint32_t native_count)
{
    return native_count - (active && native_count > inserted_header);
}

GW::UI::UIMessage GWCACompatibility::NativeUIMessage(const GW::UI::UIMessage message)
{
    const auto value = static_cast<uint32_t>(message);
    if (!active || value < 0x10000113 || value > 0x100001d0) return message;
    return static_cast<GW::UI::UIMessage>(value + 1 + (value >= 0x10000147) + (value >= 0x1000017f));
}
