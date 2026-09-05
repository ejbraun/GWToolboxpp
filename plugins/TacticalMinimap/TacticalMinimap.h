#pragma once
#include <ToolboxUIPlugin.h>
#include <GWCA/Managers/UIMgr.h>
#include "TacticalView.h"
#include <mutex>
#include <deque>
#include <unordered_map>

class TacticalMinimap final : public ToolboxUIPlugin {
public:
    TacticalMinimap();
    const char* Name() const override { return "Tactical Minimap"; }
    const char* Icon() const override;
    void Initialize(ImGuiContext*, ImGuiAllocFns, HMODULE) override;
    void SignalTerminate() override;
    void Terminate() override;
    void Update(float) override;
    void Draw(IDirect3DDevice9*) override;
    bool WndProc(UINT, WPARAM, LPARAM) override;
    void DrawSettings() override;
    void LoadSettings(const wchar_t*) override;
    void SaveSettings(const wchar_t*) override;

private:
    struct Agent { uint32_t id; Tactical::Point position; uint32_t color; bool living; };
    struct Mark { Tactical::Point from, to; uint64_t time; bool ping; };
    struct Event { UINT message; int x, y, wheel; Tactical::Input mode; bool pan, move; };
    void SendDrawing();
    GW::HookEntry compass_hook_;
    std::recursive_mutex mutex_;
    Tactical::View view_;
    std::vector<std::array<Tactical::Point, 4>> terrain_;
    std::vector<Agent> agents_;
    std::deque<Mark> marks_;
    std::deque<Event> events_;
    std::unordered_map<uint32_t, std::pair<uint32_t, Tactical::Point>> drawing_tails_;
    std::vector<GW::UI::CompassPoint> drawing_;
    Tactical::Point player_, pan_, drag_start_, draw_last_;
    ImVec2 bounds_min_, bounds_max_;
    uint32_t map_id_ = 0, map_time_ = 0, target_ = 0, player_id_ = 0, session_id_ = 0;
    float camera_angle_ = 0.f, player_angle_ = 0.f, zoom_ = 1.f;
    int opacity_ = 100, map_modifier_ = 3, target_modifier_ = 1;
    int drag_modifier_ = 2, move_modifier_ = 0;
    bool tactical_ = true, rotate_ = false, show_ranges_ = true, reset_position_ = false;
    bool active_ = false, stopping_ = false, hovered_ = false, captured_ = false, dragging_ = false;
    bool map_drag_ = false, moving_ = false, window_drag_ = false;
    uint64_t last_send_ = 0;
};
