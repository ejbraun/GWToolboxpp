#include "stdafx.h"

#include <GWCA/Utilities/Debug.h>
#include <GWCA/Utilities/Hooker.h>
#include <GWCA/Utilities/Scanner.h>

#include <atomic>
#include <limits>
#include <GWCA/Managers/MemoryMgr.h>

#include <Defines.h>
#include <ImGuiAddons.h>
#include "MouseFix.h"

#include <GWCA/Managers/UIMgr.h>

namespace {

    typedef void(__fastcall* ChangeCursorIcon_pt)(void* ctx, int edx, uint32_t cursor_type, void* bitmap_data, void* bitmap_mask, uint32_t* hotspot);

    struct GuildWarsWindowContext_vTable {
        void* h0000;
        void* h0004;
        void(__fastcall* MonitorFromWindow)(int param_1); // Calls MonitorFromWindow API
        void* h000C;
        void* h0010;
        void* h0014;
        void* h0018;
        void* h001C;
        void* h0020;
        void* h0024;
        void* h0028;
        void(__fastcall* ClearCursor_pt)(int param_1); // Destroys cursor, clears class cursor
        ChangeCursorIcon_pt ChangeCursorIcon;
    };

    struct Win32WindowUserData {
        static Win32WindowUserData* Instance() { return (Win32WindowUserData*)GetWindowLongA(GW::MemoryMgr::GetGWWindowHandle(), -0x15); }
        GuildWarsWindowContext_vTable* vtable; // Offset 0x00 - Vtable pointer
        DWORD param2;          // Offset 0x04 - Flags/parameters
        DWORD param3;          // Offset 0x08 - Parameters
        DWORD param4;          // Offset 0x0C - Parameters
        DWORD encoding_state;  // Offset 0x10 - Unicode/ANSI state
        DWORD window_state;    // Offset 0x14 - Window state (init to 2)
        DWORD field_18;        // Offset 0x18 - Reserved/unused
        HWND window_handle;    // Offset 0x1C - Window handle
        DWORD field_20;        // Offset 0x20 - Reserved/unused
        HCURSOR custom_cursor; // Offset 0x24 - Custom cursor handle ← NEW!

        // ... gap to 0x50 ...
        DWORD mouse_settings; // Offset 0x50 - Mouse configuration
        BYTE settings_flags;  // Offset 0x54 - Various bit flags

        // ... rest of 815-byte structure ...
    };

    using OnProcessInput_pt = bool(__cdecl*)(uint32_t* wParam, uint32_t* lParam);
    OnProcessInput_pt ProcessInput_Func = nullptr;
    OnProcessInput_pt ProcessInput_Ret = nullptr;

    struct GwMouseMove {
        int center_x;                // 0x00 viewport centre the camera deltas are measured against
        int center_y;                // 0x04
        int captured_client_x;       // 0x08 cursor position in client space when the camera was captured
        int captured_client_y;       // 0x0c
        uint32_t unk;                // 0x10
        uint32_t mouse_button_state; // 0x14 0x1 - LMB, 0x2 - MMB, 0x4 - RMB
        uint32_t move_camera;        // 0x18 1 == control camera while right mouse button pressed
        int captured_x;              // 0x1c cursor position in screen space when the camera was captured
        int captured_y;              // 0x20
        // 0x24 unused, 0x28 has_registered_track_mouse_event
    };

    GwMouseMove* gw_mouse_move = nullptr;
    // ArenaNet shuffles OsInput event IDs between builds; the native handler now supplies the camera event.
    bool* HasRegisteredTrackMouseEvent = nullptr;
    using SetCursorPosCenter_pt = void(__cdecl*)(GwMouseMove* wParam);
    SetCursorPosCenter_pt SetCursorPosCenter_Func = nullptr;
    SetCursorPosCenter_pt SetCursorPosCenter_Ret = nullptr;    // Override (and rewrite) GW's handling of setting the mouse cursor to the center of the screen (bypass GameMutex, may be the cause of camera glitch)
    // This could be a patch really, but rewriting the function out is a bit more readable.
    
    bool initialized = false;
    MouseFix::Settings settings;

    bool ShouldFixCursor() {
        return settings.enable_cursor_fix && !GW::UI::IsInControllerMode();
    }
    HCURSOR current_cursor = nullptr;
    bool cursor_size_hooked = false;
    
    void OnSetCursorPosCenter(GwMouseMove* gwmm)
    {
        GW::Hook::EnterHook();
        const auto gw_window_handle = GW::MemoryMgr::GetGWWindowHandle();
        // @Enhancement: Maybe assert that gwmm == gw_mouse_move?
        // @Enhancement: Maybe check that the focussed window handle is the GW window handle?
        RECT rect{};
        POINT center{};
        if (ShouldFixCursor() && gwmm && gwmm == gw_mouse_move && gw_window_handle
            && GetForegroundWindow() == gw_window_handle && GetClientRect(gw_window_handle, &rect)) {
            center = {(rect.left + rect.right) / 2, (rect.top + rect.bottom) / 2};
            if (ClientToScreen(gw_window_handle, &center) && SetCursorPos(center.x, center.y)) {
                gwmm->center_x = center.x;
                gwmm->center_y = center.y;
                GW::Hook::LeaveHook();
                return;
            }
        }
        if (gwmm && SetCursorPosCenter_Ret) {
            SetCursorPosCenter_Ret(gwmm);
        }
        GW::Hook::LeaveHook();
    }

    // Override (and rewrite) GW's handling of mouse event 0x200 to stop camera glitching.
    bool OnProcessInput(uint32_t* wParam, uint32_t* lParam)
    {
        GW::Hook::EnterHook();
        if (!(wParam && lParam && ProcessInput_Ret)) {
            GW::Hook::LeaveHook();
            return false;
        }
        auto input = wParam;
        std::array<uint32_t, 4> current_message{};
        if (ShouldFixCursor() && HasRegisteredTrackMouseEvent && gw_mouse_move
            && wParam[1] == WM_MOUSEMOVE && *HasRegisteredTrackMouseEvent && gw_mouse_move->move_camera) {
            const auto hwnd = GW::MemoryMgr::GetGWWindowHandle();
            POINT cursor{};
            if (hwnd && reinterpret_cast<HWND>(wParam[0]) == hwnd && GetForegroundWindow() == hwnd
                && GetCursorPos(&cursor) && ScreenToClient(hwnd, &cursor)
                && cursor.x >= std::numeric_limits<short>::min() && cursor.x <= std::numeric_limits<short>::max()
                && cursor.y >= std::numeric_limits<short>::min() && cursor.y <= std::numeric_limits<short>::max()) {
                // Queued WM_MOUSEMOVE coordinates can predate a cursor warp and produce a false camera delta.
                current_message = {wParam[0], wParam[1], wParam[2], static_cast<uint32_t>(MAKELPARAM(cursor.x, cursor.y))};
                input = current_message.data();
            }
        }
        // Native ClientToScreen and viewport recentering preserve Windows pointer speed, acceleration and DPI handling.
        const auto result = ProcessInput_Ret(input, lParam);
        GW::Hook::LeaveHook();
        return result;
    }

    bool CursorFixInitialise()
    {
        if (initialized) {
            return true;
        }
        const auto hwnd = GW::MemoryMgr::GetGWWindowHandle();
        if (!hwnd) {
            return false;
        }
        uintptr_t address = GW::Scanner::Find("\xc7\x45\xf0\x10\x00\x00\x00\xc7\x45\xf4\x02\x00\x00\x00", "xx?xxxxxx?xxxx", 0x15);
        DEBUG_ASSERT(address);
        if(address && GW::Scanner::IsValidPtr(*(uintptr_t*)address)) {
            ProcessInput_Func = (OnProcessInput_pt)GW::Scanner::ToFunctionStart(address, 0xfff);

            address = GW::Scanner::FindInRange("\x83\x3d????\x00", "xx????x", 2, address, address - 0x30);
            if (address && GW::Scanner::IsValidPtr(*(uintptr_t*)address)) {
                HasRegisteredTrackMouseEvent = *(bool**)address;
                gw_mouse_move = (GwMouseMove*)(HasRegisteredTrackMouseEvent - 0x28);
            }
        }
        // The former fixed 0x11 value followed a failed event-ID scan; native dispatch avoids that dependency.
        SetCursorPosCenter_Func = (SetCursorPosCenter_pt)GW::Scanner::ToFunctionStart(GW::Scanner::FindAssertion("OsInput.cpp", "basis", 0, 0));
        DEBUG_ASSERT(ProcessInput_Func);
        DEBUG_ASSERT(SetCursorPosCenter_Func);
        DEBUG_ASSERT(HasRegisteredTrackMouseEvent);
        DEBUG_ASSERT(gw_mouse_move);

        GWCA_INFO("[SCAN] ProcessInput_Func = %p", ProcessInput_Func);
        GWCA_INFO("[SCAN] HasRegisteredTrackMouseEvent = %p", HasRegisteredTrackMouseEvent);
        GWCA_INFO("[SCAN] gw_mouse_move = %p", gw_mouse_move);
        GWCA_INFO("[SCAN] SetCursorPosCenter_Func = %p", SetCursorPosCenter_Func);

#ifdef _DEBUG
        //ASSERT(ProcessInput_Func && HasRegisteredTrackMouseEvent && gw_mouse_move && SetCursorPosCenter_Func);
#endif
        if (!(ProcessInput_Func && HasRegisteredTrackMouseEvent && gw_mouse_move && SetCursorPosCenter_Func)) {
            return false;
        }
        if (GW::Hook::CreateHook((void**)&ProcessInput_Func, OnProcessInput, reinterpret_cast<void**>(&ProcessInput_Ret)) != 0) {
            return false;
        }
        if (GW::Hook::CreateHook((void**)&SetCursorPosCenter_Func, OnSetCursorPosCenter, reinterpret_cast<void**>(&SetCursorPosCenter_Ret)) != 0) {
            GW::Hook::RemoveHook(ProcessInput_Func);
            ProcessInput_Ret = nullptr;
            return false;
        }
        initialized = true;
        return true;
    }

    void CursorFixEnable(const bool enable)
    {
        if (enable && CursorFixInitialise()) {
            GW::Hook::EnableHooks(ProcessInput_Func);
            GW::Hook::EnableHooks(SetCursorPosCenter_Func);
        }
        else if (initialized) {
            GW::Hook::DisableHooks(ProcessInput_Func);
            GW::Hook::DisableHooks(SetCursorPosCenter_Func);
        }
    }

    HBITMAP ScaleBitmap(const HBITMAP inBitmap, const int inWidth, const int inHeight, const int outWidth, const int outHeight)
    {
        // NB: We could use GDIPlus for this logic which has better image res handling etc, but no need
        HDC srcDC = nullptr;
        BYTE* ppvBits = nullptr;
        BOOL bResult = 0;
        HBITMAP outBitmap = nullptr;
        HGDIOBJ oldDestBitmap = nullptr, oldSrcBitmap = nullptr;

        BITMAPINFO bmi;
        memset(&bmi, 0, sizeof(bmi));
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biWidth = outWidth;
        bmi.bmiHeader.biHeight = outHeight;
        bmi.bmiHeader.biPlanes = 1;

        // Do not use CreateCompatibleBitmap otherwise api will not allocate memory for bitmap
        const HDC destDC = CreateCompatibleDC(nullptr);
        if (!destDC) {
            goto cleanup;
        }
        outBitmap = CreateDIBSection(destDC, &bmi, DIB_RGB_COLORS, (void**)&ppvBits, nullptr, 0);
        if (outBitmap == nullptr) {
            goto cleanup;
        }
        oldDestBitmap = SelectObject(destDC, outBitmap);
        if (oldDestBitmap == nullptr) {
            goto cleanup;
        }

        srcDC = CreateCompatibleDC(nullptr);
        if (!srcDC) {
            goto cleanup;
        }
        oldSrcBitmap = SelectObject(srcDC, inBitmap);
        if (oldSrcBitmap == nullptr) {
            goto cleanup;
        }

        if (SetStretchBltMode(destDC, WHITEONBLACK) == 0) {
            goto cleanup;
        }
        bResult = StretchBlt(destDC, 0, 0, outWidth, outHeight, srcDC, 0, 0, inWidth, inHeight, SRCCOPY);
    cleanup:
        // a bitmap still selected into a DC can't be deleted, so restore the originals first
        if (oldDestBitmap) {
            SelectObject(destDC, oldDestBitmap);
        }
        if (oldSrcBitmap) {
            SelectObject(srcDC, oldSrcBitmap);
        }
        if (!bResult) {
            if (outBitmap) {
                DeleteObject(outBitmap);
                outBitmap = nullptr;
            }
        }
        if (destDC) {
            DeleteDC(destDC);
        }
        if (srcDC) {
            DeleteDC(srcDC);
        }

        return outBitmap;
    }


    HCURSOR ScaleCursor(const HCURSOR cursor, const int targetSize)
    {
        ICONINFO icon_info = { 0 };
        HCURSOR new_cursor = nullptr;
        BITMAP tmpBitmap = { 0 };
        HBITMAP scaledMask = nullptr, scaledColor = nullptr;
        if (!GetIconInfo(cursor, &icon_info)) {
            goto cleanup;
        }
        if (GetObject(icon_info.hbmMask, sizeof(BITMAP), &tmpBitmap) == 0) {
            goto cleanup;
        }
        if (!(tmpBitmap.bmHeight && tmpBitmap.bmWidth))
            goto cleanup;
        if (tmpBitmap.bmWidth == targetSize) {
            goto cleanup;
        }
        scaledMask = ScaleBitmap(icon_info.hbmMask, tmpBitmap.bmWidth, tmpBitmap.bmHeight, targetSize, targetSize);
        if (!scaledMask) {
            goto cleanup;
        }
        if (GetObject(icon_info.hbmColor, sizeof(BITMAP), &tmpBitmap) == 0) {
            goto cleanup;
        }
        scaledColor = ScaleBitmap(icon_info.hbmColor, tmpBitmap.bmWidth, tmpBitmap.bmHeight, targetSize, targetSize);
        if (!scaledColor) {
            goto cleanup;
        }
        {
            // CreateIconIndirect copies these, so the scaled bitmaps are still ours to free below
            ICONINFO scaled_icon_info = icon_info;
            scaled_icon_info.hbmColor = scaledColor;
            scaled_icon_info.hbmMask = scaledMask;
            new_cursor = CreateIconIndirect(&scaled_icon_info);
        }
    cleanup:
        // GetIconInfo hands out private copies of the bitmaps; failing to free them leaks 2 GDI objects per cursor change
        if (icon_info.hbmColor)
            DeleteObject(icon_info.hbmColor);
        if (icon_info.hbmMask)
            DeleteObject(icon_info.hbmMask);
        if (scaledColor)
            DeleteObject(scaledColor);
        if (scaledMask)
            DeleteObject(scaledMask);
        return new_cursor;
    }


    ChangeCursorIcon_pt ChangeCursorIcon_Func = nullptr, ChangeCursorIcon_Ret = nullptr;

    struct CachedCursorData {
        uint32_t cursor_type = 0;
        std::array<uint8_t, 32 * 32 * 4> bitmap_data{};
        std::array<uint8_t, 32 * 32> bitmap_mask{};
        uint32_t hotspot[2]{};
        bool is_valid = false;
    };

    CachedCursorData cached_cursor;
    std::mutex cursor_mutex;
    std::atomic<bool> redraw_cursor_pending = false;

    void ApplyCursorIcon(Win32WindowUserData* user_data, uint32_t edx, uint32_t cursor_type, void* bitmap_data, void* bitmap_mask, uint32_t* hotspot)
    {
        const std::scoped_lock lock(cursor_mutex);
        if (!(user_data && ChangeCursorIcon_Ret)) {
            return;
        }
        cached_cursor.is_valid = false;
        if (bitmap_data && bitmap_mask && hotspot && (cursor_type == 0 || cursor_type == 5)) {
            cached_cursor.cursor_type = cursor_type;

            size_t bitmap_size;
            if (cursor_type == 0) {
                bitmap_size = 32 * 32 * 4; // 32-bit color (RGBA)
            }
            else {
                bitmap_size = 32 * 32 * 2; // 16-bit color
            }
            // Other formats are not supported by the native cursor creator; do not guess a 32-bit size.
            if (bitmap_data != cached_cursor.bitmap_data.data()) {
                memcpy(cached_cursor.bitmap_data.data(), bitmap_data, bitmap_size);
            }
            // GW supplies one byte per mask pixel and packs it into a monochrome bitmap itself.
            if (bitmap_mask != cached_cursor.bitmap_mask.data()) {
                memcpy(cached_cursor.bitmap_mask.data(), bitmap_mask, cached_cursor.bitmap_mask.size());
            }
            cached_cursor.hotspot[0] = hotspot[0];
            cached_cursor.hotspot[1] = hotspot[1];
            cached_cursor.is_valid = true;
        }

        ChangeCursorIcon_Ret(user_data, edx, cursor_type, bitmap_data, bitmap_mask, hotspot);

        if (settings.cursor_size < 16 || settings.cursor_size > 64 || settings.cursor_size == 32) {
            return;
        }


        HCURSOR* cursor = &user_data->custom_cursor;
        HWND* window_handle = &user_data->window_handle;

        if (!(user_data && *cursor && *cursor != current_cursor)) {
            return;
        }
        const HCURSOR new_cursor = ScaleCursor(*cursor, settings.cursor_size);
        if (!new_cursor) {
            return;
        }
        if (*cursor == new_cursor) {
            return;
        }
        if (*cursor) {
            // Don't forget to free the original cursor before overwriting the handle
            DestroyCursor(*cursor);
            SetClassLongA(*window_handle, GCL_HCURSOR, 0);
            SetCursor(nullptr);
            *cursor = nullptr;
        }
        *cursor = new_cursor;
        SetCursor(new_cursor);
        SetClassLongA(*window_handle, GCL_HCURSOR, reinterpret_cast<LONG>(new_cursor));
        current_cursor = new_cursor;
    }

    void __fastcall OnChangeCursorIcon(Win32WindowUserData* user_data, uint32_t edx, uint32_t cursor_type, void* bitmap_data, void* bitmap_mask, uint32_t* hotspot)
    {
        GW::Hook::EnterHook();
        ApplyCursorIcon(user_data, edx, cursor_type, bitmap_data, bitmap_mask, hotspot);
        GW::Hook::LeaveHook();
    }

    void RedrawCursorIcon()
    {
        // Update owns this request so no queued callback can outlive the module.
        redraw_cursor_pending = true;
    }

    void SetCursorSize(const int new_size)
    {
        settings.cursor_size = std::clamp(new_size, 16, 64);
        RedrawCursorIcon();
    }

    GW::HookEntry UIMessage_HookEntry;

    void OnUIMessage(GW::HookStatus*, GW::UI::UIMessage message_id, void*, void*) {
        switch (message_id) {
        case GW::UI::UIMessage::kLogout:
            CursorFixEnable(false);
            break;
        case GW::UI::UIMessage::kMapLoaded:
            CursorFixEnable(settings.enable_cursor_fix);
            break;
        }
    }

} // namespace

void MouseFix::Initialize()
{
    ToolboxModule::Initialize();
    SettingsRegistry::Register(this, settings);

    ChangeCursorIcon_Func = (ChangeCursorIcon_pt)GW::Scanner::ToFunctionStart(GW::Scanner::Find("\x80\x7e\x01\x80", "xxxx"));
    if (ChangeCursorIcon_Func
        && GW::Hook::CreateHook((void**)&ChangeCursorIcon_Func, OnChangeCursorIcon, (void**)&ChangeCursorIcon_Ret) == 0) {
        cursor_size_hooked = true;
        GW::Hook::EnableHooks(ChangeCursorIcon_Func);
    }

#if _DEBUG
    ASSERT(ChangeCursorIcon_Func);
#endif

    const GW::UI::UIMessage ui_messages[] = {
        GW::UI::UIMessage::kLogout,
        GW::UI::UIMessage::kMapLoaded
    };

    for (const auto ui_message : ui_messages) {
        RegisterUIMessageCallback(&UIMessage_HookEntry, ui_message, OnUIMessage);
    }
}

void MouseFix::LoadSettings(SettingsDoc& doc, ToolboxIni* legacy)
{
    ToolboxModule::LoadSettings(doc, legacy);
    doc.GetStruct(Name(), settings);
    SetCursorSize(settings.cursor_size);
    CursorFixEnable(settings.enable_cursor_fix);
}

void MouseFix::SaveSettings(SettingsDoc& doc)
{
    ToolboxModule::SaveSettings(doc);
    doc.SetStruct(Name(), settings);
}

void MouseFix::Update(float)
{
    if (!redraw_cursor_pending.exchange(false)) return;
    CachedCursorData cursor_data;
    {
        const std::scoped_lock lock(cursor_mutex);
        if (!(cursor_size_hooked && cached_cursor.is_valid)) return;
        cursor_data = cached_cursor;
        current_cursor = nullptr;
    }
    if (const auto user_data = Win32WindowUserData::Instance(); user_data && ChangeCursorIcon_Func) {
        ChangeCursorIcon_Func(user_data, 0, cursor_data.cursor_type,
                             cursor_data.bitmap_data.data(), cursor_data.bitmap_mask.data(), cursor_data.hotspot);
    }
}

void MouseFix::Terminate()
{
    ToolboxModule::Terminate();
    CursorFixEnable(false);
    GW::UI::RemoveUIMessageCallback(&UIMessage_HookEntry);
    if (initialized) {
        GW::Hook::RemoveHook(ProcessInput_Func);
        GW::Hook::RemoveHook(SetCursorPosCenter_Func);
    }
    if (cursor_size_hooked) {
        GW::Hook::DisableHooks(ChangeCursorIcon_Func);
        GW::Hook::RemoveHook(ChangeCursorIcon_Func);
    }
    const std::scoped_lock lock(cursor_mutex);
    redraw_cursor_pending = false;
    cached_cursor = {};
    current_cursor = nullptr;
    cursor_size_hooked = initialized = false;
    ProcessInput_Func = ProcessInput_Ret = nullptr;
    SetCursorPosCenter_Func = SetCursorPosCenter_Ret = nullptr;
    ChangeCursorIcon_Func = ChangeCursorIcon_Ret = nullptr;
    HasRegisteredTrackMouseEvent = nullptr;
    gw_mouse_move = nullptr;
}

void MouseFix::DrawSettingsInternal()
{
    if (ImGui::Checkbox("Enable cursor fix", &settings.enable_cursor_fix)) {
        CursorFixEnable(settings.enable_cursor_fix);
    }
    ImGui::SliderInt("Guild Wars cursor size", &settings.cursor_size, 16, 64);
    ImGui::ShowHelp("Sizes other than 32 might lead the the cursor disappearing at random.\n"
        "Right click to make the cursor dis- and reappear for this to take effect.");
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        SetCursorSize(settings.cursor_size);
    }
    if (ImGui::Button("Reset")) {
        SetCursorSize(32);
    }
}

bool MouseFix::WndProc(const UINT, const WPARAM, const LPARAM)
{
    if (ShouldFixCursor() && !initialized) {
        CursorFixEnable(settings.enable_cursor_fix);
    }
    return false;
}
