#include "TacticalMinimap.h"
#include <IconsFontAwesome5.h>
#include <GWCA/Constants/Constants.h>
#include <GWCA/GameEntities/Agent.h>
#include <GWCA/GameEntities/Camera.h>
#include <GWCA/GameEntities/Pathing.h>
#include <GWCA/Managers/AgentMgr.h>
#include <GWCA/Managers/CameraMgr.h>
#include <GWCA/Managers/GameThreadMgr.h>
#include <GWCA/Managers/MapMgr.h>
#include <GWCA/Managers/PlayerMgr.h>
#include <GWCA/Managers/MemoryMgr.h>
#include <windowsx.h>

namespace {
    constexpr int keys[] = {0, VK_CONTROL, VK_SHIFT, VK_MENU};
    constexpr const char* modifiers[] = {"None", "Ctrl", "Shift", "Alt"};
    bool Held(const int modifier) { return modifier > 0 && modifier < 4 && (GetKeyState(keys[modifier]) & 0x8000); }
    ImVec2 Pixel(const Tactical::Point point) { return {point.x, point.y}; }
}

TacticalMinimap::TacticalMinimap()
{
    can_show_in_main_window = true;
    show_title = false;
    lock_move = true;
    lock_size = true;
}

const char* TacticalMinimap::Icon() const { return ICON_FA_MAP; }

void TacticalMinimap::Initialize(ImGuiContext* context, const ImGuiAllocFns allocators, const HMODULE toolbox)
{
    ToolboxUIPlugin::Initialize(context, allocators, toolbox);
    session_id_ = GetTickCount();
    GW::UI::RegisterUIMessageCallback(&compass_hook_, GW::UI::UIMessage::kCompassDraw,
        [this](GW::HookStatus*, GW::UI::UIMessage, void* parameter, void*) {
            std::scoped_lock lock(mutex_);
            if (stopping_ || !parameter) return;
            const auto packet = static_cast<GW::UI::UIPacket::kCompassDraw*>(parameter);
            if (!packet->points || !packet->number_of_points || packet->number_of_points > 8) return;
            const auto now = GetTickCount64();
            if (drawing_tails_.size() > 256) drawing_tails_.clear();
            auto& tail = drawing_tails_[packet->player_number];
            const auto fresh = tail.first != packet->session_id;
            for (auto i = uint32_t{0}; i < packet->number_of_points; ++i) {
                const Tactical::Point point{static_cast<float>(packet->points[i].x) * 96.f, static_cast<float>(packet->points[i].y) * 96.f};
                if (fresh && i == 0) {
                    if (packet->number_of_points == 1) marks_.push_back({point, point, now, true});
                }
                else marks_.push_back({tail.second, point, now, false});
                tail = {packet->session_id, point};
            }
            while (marks_.size() > 4096) marks_.pop_front();
        });
}

void TacticalMinimap::SignalTerminate()
{
    std::scoped_lock lock(mutex_);
    stopping_ = true;
    active_ = hovered_ = captured_ = dragging_ = false;
    events_.clear();
    drawing_.clear();
    GW::UI::RemoveUIMessageCallback(&compass_hook_);
    ToolboxUIPlugin::SignalTerminate();
}

void TacticalMinimap::Terminate()
{
    std::scoped_lock lock(mutex_);
    terrain_.clear();
    agents_.clear();
    marks_.clear();
    ToolboxUIPlugin::Terminate();
}

void TacticalMinimap::Update(float)
{
    std::scoped_lock lock(mutex_);
    if (stopping_) return;
    const auto now = GetTickCount64();
    while (!marks_.empty() && now - marks_.front().time > 5000) marks_.pop_front();
    const auto player = GW::Agents::GetControlledCharacter();
    active_ = GW::Map::GetIsMapLoaded() && player && !GW::UI::GetIsWorldMapShowing() && !GW::Map::GetIsInCinematic();
    const auto map_id = static_cast<uint32_t>(GW::Map::GetMapID());
    const auto map_time = GW::Map::GetInstanceTime();
    if (!active_ || map_id_ != map_id || map_time < map_time_) {
        terrain_.clear();
        agents_.clear();
        marks_.clear();
        drawing_tails_.clear();
        drawing_.clear();
        events_.clear();
        pan_ = {};
        captured_ = dragging_ = hovered_ = false;
    }
    map_id_ = map_id;
    map_time_ = map_time;
    if (!active_) return;
    player_ = {player->pos.x, player->pos.y};
    player_angle_ = player->rotation_angle;
    player_id_ = player->agent_id;
    target_ = GW::Agents::GetTargetId();
    if (const auto camera = GW::CameraMgr::GetCamera()) camera_angle_ = camera->yaw;
    if (terrain_.empty()) {
        if (const auto pathing = GW::Map::GetPathingMap(); pathing && pathing->valid()) {
            for (const auto& layer : *pathing) {
                if (!layer.trapezoids || layer.trapezoid_count > 200000) continue;
                for (auto i = uint32_t{0}; i < layer.trapezoid_count && terrain_.size() < 200000; ++i) {
                    const auto& trap = layer.trapezoids[i];
                    terrain_.push_back({Tactical::Point{trap.XTL, trap.YT}, {trap.XTR, trap.YT}, {trap.XBR, trap.YB}, {trap.XBL, trap.YB}});
                }
            }
        }
    }
    agents_.clear();
    if (const auto agents = GW::Agents::GetAgentArray(); agents && agents->valid()) {
        for (const auto agent : *agents) {
            if (!agent) continue;
            auto color = IM_COL32(230, 220, 160, 255);
            const auto living = agent->GetAsAgentLiving();
            if (living) {
                switch (static_cast<uint32_t>(living->allegiance)) {
                    case 1: color = IM_COL32(60, 215, 110, 255); break;
                    case 2: color = IM_COL32(245, 205, 70, 255); break;
                    case 3: color = IM_COL32(240, 75, 75, 255); break;
                    default: color = IM_COL32(115, 170, 235, 255); break;
                }
                if (living->GetIsDead()) color = IM_COL32(130, 130, 130, 130);
            }
            agents_.push_back({agent->agent_id, {agent->pos.x, agent->pos.y}, color, living != nullptr});
        }
    }
}

void TacticalMinimap::SendDrawing()
{
    if (drawing_.empty()) return;
    auto points = std::move(drawing_);
    drawing_.clear();
    const auto session = session_id_;
    GW::GameThread::Enqueue([points = std::move(points), session]() mutable {
        GW::UI::DrawOnCompass(session, static_cast<unsigned>(points.size()), points.data());
        GW::UI::UIPacket::kCompassDraw packet{GW::PlayerMgr::GetPlayerNumber(), session, static_cast<uint32_t>(points.size()), points.data()};
        GW::UI::SendUIMessage(GW::UI::UIMessage::kCompassDraw, &packet);
    });
    last_send_ = GetTickCount64();
}

bool TacticalMinimap::WndProc(const UINT message, const WPARAM wparam, const LPARAM)
{
    std::scoped_lock lock(mutex_);
    if (message == WM_KILLFOCUS || message == WM_CANCELMODE || (message == WM_ACTIVATE && LOWORD(wparam) == WA_INACTIVE)) {
        captured_ = dragging_ = false;
        events_.clear();
        drawing_.clear();
    }
    return false;
}

void TacticalMinimap::Draw(IDirect3DDevice9*)
{
    std::scoped_lock lock(mutex_);
    hovered_ = false;
    if (!active_ || stopping_ || !*GetVisiblePtr()) return;
    const auto mode = Tactical::ResolveInput(tactical_, Held(map_modifier_), Held(target_modifier_));
    const auto interactive = ((mode != Tactical::Input::Passthrough && opacity_ > 0) || !lock_move)
        && !ImGui::GetIO().WantTextInput && !ImGui::IsMouseDown(ImGuiMouseButton_Right);
    auto flags = GetWinFlags(ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoFocusOnAppearing);
    if (!interactive && lock_size) flags |= ImGuiWindowFlags_NoInputs;
    ImGui::SetNextWindowSize(ImVec2(320.f, 320.f), ImGuiCond_FirstUseEver);
    if (reset_position_) {
        ImGui::SetNextWindowPos(ImVec2(80.f, 120.f));
        ImGui::SetNextWindowSize(ImVec2(320.f, 320.f));
        reset_position_ = false;
    }
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * static_cast<float>(opacity_) / 100.f);
    if (ImGui::Begin(Name(), GetVisiblePtr(), flags)) {
        bounds_min_ = ImGui::GetCursorScreenPos();
        const auto size = ImGui::GetContentRegionAvail();
        bounds_max_ = {bounds_min_.x + size.x, bounds_min_.y + size.y};
        if (interactive) ImGui::InvisibleButton("##map", size, ImGuiButtonFlags_MouseButtonLeft);
        else ImGui::Dummy(size);
        hovered_ = interactive && ImGui::IsItemHovered();
        const auto& io = ImGui::GetIO();
        const auto mouse_x = static_cast<int>(io.MousePos.x), mouse_y = static_cast<int>(io.MousePos.y);
        if (hovered_ && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            captured_ = true;
            events_.push_back({static_cast<UINT>(ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) ? WM_LBUTTONDBLCLK : WM_LBUTTONDOWN),
                mouse_x, mouse_y, 0, mode, Held(drag_modifier_), Held(move_modifier_)});
        }
        else if (captured_ && ImGui::IsMouseDown(ImGuiMouseButton_Left) && interactive && (io.MouseDelta.x || io.MouseDelta.y)) {
            events_.push_back({WM_MOUSEMOVE, mouse_x, mouse_y, 0, mode, Held(drag_modifier_), Held(move_modifier_)});
        }
        if (captured_ && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
            events_.push_back({WM_LBUTTONUP, mouse_x, mouse_y});
            captured_ = false;
        }
        if (hovered_ && mode == Tactical::Input::Map && io.MouseWheel) {
            events_.push_back({WM_MOUSEWHEEL, mouse_x, mouse_y, static_cast<int>(io.MouseWheel * WHEEL_DELTA), mode});
        }
        view_.center = {player_.x + pan_.x, player_.y + pan_.y};
        view_.screen = {(bounds_min_.x + bounds_max_.x) * .5f, (bounds_min_.y + bounds_max_.y) * .5f};
        view_.scale = .05f * zoom_;
        view_.angle = rotate_ ? camera_angle_ - 1.570796327f : 0.f;
        for (const auto& event : events_) {
            const Tactical::Point pixel{static_cast<float>(event.x), static_cast<float>(event.y)};
            const auto world = view_.ToWorld(pixel);
            if (event.message == WM_LBUTTONUP) {
                if (dragging_ && !map_drag_ && !moving_ && !window_drag_) SendDrawing();
                dragging_ = false;
                continue;
            }
            if (event.message == WM_MOUSEWHEEL) {
                zoom_ = std::clamp(zoom_ * std::pow(1.1f, static_cast<float>(event.wheel) / WHEEL_DELTA), .15f, 8.f);
                continue;
            }
            if (event.mode == Tactical::Input::Target) {
                if (dragging_) { SendDrawing(); dragging_ = false; }
                const Agent* closest = nullptr;
                auto distance = 18.f * 18.f;
                for (const auto& agent : agents_) {
                    if (!agent.living || agent.id == player_id_) continue;
                    const auto pos = view_.ToScreen(agent.position);
                    const auto squared = (pos.x - pixel.x) * (pos.x - pixel.x) + (pos.y - pixel.y) * (pos.y - pixel.y);
                    if (squared < distance) { closest = &agent; distance = squared; }
                }
                if (closest) GW::GameThread::Enqueue([id = closest->id] { GW::Agents::ChangeTarget(id); });
                continue;
            }
            if (event.message == WM_LBUTTONDBLCLK && event.pan) { pan_ = {}; continue; }
            if (event.message == WM_LBUTTONDOWN || event.message == WM_LBUTTONDBLCLK) {
                dragging_ = true;
                map_drag_ = event.pan;
                moving_ = event.move;
                window_drag_ = !lock_move && !map_drag_ && !moving_;
                drag_start_ = pixel;
                draw_last_ = world;
                if (moving_) {
                    GW::GameThread::Enqueue([world] { GW::Agents::Move(GW::GamePos(world.x, world.y, 0)); });
                }
                else if (!map_drag_ && !window_drag_) {
                    ++session_id_;
                    drawing_.push_back({static_cast<int>(world.x / 96.f), static_cast<int>(world.y / 96.f)});
                }
            }
            else if (dragging_ && event.message == WM_MOUSEMOVE) {
                if (window_drag_) {
                    const auto position = ImGui::GetWindowPos();
                    ImGui::SetWindowPos(ImVec2(position.x + pixel.x - drag_start_.x, position.y + pixel.y - drag_start_.y));
                    drag_start_ = pixel;
                }
                else if (map_drag_) {
                    const auto previous = view_.ToWorld(drag_start_);
                    pan_.x += previous.x - world.x;
                    pan_.y += previous.y - world.y;
                    drag_start_ = pixel;
                }
                else if (!moving_) {
                    const auto dx = world.x - draw_last_.x, dy = world.y - draw_last_.y;
                    if (dx * dx + dy * dy < 96.f * 96.f) continue;
                    drawing_.push_back({static_cast<int>(world.x / 96.f), static_cast<int>(world.y / 96.f)});
                    draw_last_ = world;
                    if (drawing_.size() >= 7 || GetTickCount64() - last_send_ > 150) SendDrawing();
                }
            }
        }
        events_.clear();
        if ((!interactive || !ImGui::IsMouseDown(ImGuiMouseButton_Left)) && dragging_) { SendDrawing(); dragging_ = captured_ = false; }
        auto* draw = ImGui::GetWindowDrawList();
        draw->PushClipRect(bounds_min_, bounds_max_, true);
        const auto terrain_color = ImGui::GetColorU32(ImVec4(.30f, .34f, .37f, .90f));
        for (const auto& trap : terrain_) {
            ImVec2 polygon[4];
            auto min_x = FLT_MAX, min_y = FLT_MAX, max_x = -FLT_MAX, max_y = -FLT_MAX;
            for (auto i = size_t{0}; i < 4; ++i) {
                polygon[i] = Pixel(view_.ToScreen(trap[i]));
                min_x = std::min(min_x, polygon[i].x); min_y = std::min(min_y, polygon[i].y);
                max_x = std::max(max_x, polygon[i].x); max_y = std::max(max_y, polygon[i].y);
            }
            if (max_x < bounds_min_.x || max_y < bounds_min_.y || min_x > bounds_max_.x || min_y > bounds_max_.y) continue;
            draw->AddConvexPolyFilled(polygon, 4, terrain_color);
        }
        const auto player_pixel = Pixel(view_.ToScreen(player_));
        if (show_ranges_) {
            draw->AddCircle(player_pixel, 1012.f * view_.scale, ImGui::GetColorU32(ImVec4(1.f, .65f, .2f, .65f)), 64);
            draw->AddCircle(player_pixel, 1248.f * view_.scale, ImGui::GetColorU32(ImVec4(.7f, .8f, 1.f, .45f)), 64);
        }
        for (const auto& agent : agents_) {
            const auto pos = Pixel(view_.ToScreen(agent.position));
            if (agent.id == player_id_) continue;
            if (agent.id == target_) draw->AddCircle(pos, 7.f, ImGui::GetColorU32(ImVec4(1.f, 1.f, 1.f, 1.f)), 16, 2.f);
            draw->AddCircleFilled(pos, agent.living ? 3.f : 2.f, ImGui::GetColorU32(agent.color), 12);
        }
        ImVec2 triangle[3];
        const auto direction = player_angle_ - view_.angle;
        for (auto i = 0; i < 3; ++i) {
            const auto angle = direction + static_cast<float>(i) * 2.094395102f;
            const auto radius = i == 0 ? 8.f : 5.f;
            triangle[i] = {player_pixel.x + std::cos(angle) * radius, player_pixel.y - std::sin(angle) * radius};
        }
        draw->AddTriangleFilled(triangle[0], triangle[1], triangle[2], ImGui::GetColorU32(ImVec4(1.f, 1.f, 1.f, 1.f)));
        const auto now = GetTickCount64();
        for (const auto& mark : marks_) {
            const auto age = static_cast<float>(now - mark.time);
            const auto color = ImGui::GetColorU32(ImVec4(1.f, .25f, .2f, std::clamp(1.f - age / 5000.f, 0.f, 1.f)));
            if (mark.ping) draw->AddCircle(Pixel(view_.ToScreen(mark.from)), 6.f + std::fmod(age / 40.f, 20.f), color, 24, 2.f);
            else draw->AddLine(Pixel(view_.ToScreen(mark.from)), Pixel(view_.ToScreen(mark.to)), color, 2.f);
        }
        draw->PopClipRect();
    }
    ImGui::End();
    ImGui::PopStyleVar();
}

void TacticalMinimap::DrawSettings()
{
    std::scoped_lock lock(mutex_);
    ToolboxUIPlugin::DrawSettings();
    ImGui::Checkbox("Enable tactical mode", &tactical_);
    ImGui::SliderInt("Minimap opacity", &opacity_, 0, 100, "%d%%");
    ImGui::Checkbox("Rotate with camera", &rotate_);
    ImGui::Checkbox("Show aggro and spellcast ranges", &show_ranges_);
    ImGui::SliderFloat("Zoom", &zoom_, .15f, 8.f, "%.2fx");
    ImGui::Combo("Map interaction", &map_modifier_, modifiers, 4);
    ImGui::Combo("Target interaction", &target_modifier_, modifiers, 4);
    ImGui::Combo("Pan map (during map interaction)", &drag_modifier_, modifiers, 4);
    ImGui::Combo("Move character (during map interaction)", &move_modifier_, modifiers, 4);
    ImGui::TextWrapped("Tactical mode passes mouse clicks through until a modifier is held. Targeting wins when both modifiers are held. Hold the map modifier to ping, draw or zoom; add the pan modifier to drag the view. Double-click with the pan modifier to recenter. None disables a modifier action.");
    ImGui::TextWrapped("Unlock movement or resizing above to reposition or resize this window without holding a modifier. These settings apply only to the DBBox Tactical Minimap.");
    if (ImGui::Button("Recenter")) pan_ = {};
    ImGui::SameLine();
    if (ImGui::Button("Reset window position")) reset_position_ = true;
}

void TacticalMinimap::LoadSettings(const wchar_t* folder)
{
    std::scoped_lock lock(mutex_);
    ToolboxUIPlugin::LoadSettings(folder);
    LoadSetting("tactical_mode", tactical_);
    LoadSetting("opacity", opacity_);
    LoadSetting("rotate", rotate_);
    LoadSetting("ranges", show_ranges_);
    LoadSetting("zoom", zoom_);
    LoadSetting("map_modifier", map_modifier_);
    LoadSetting("target_modifier", target_modifier_);
    LoadSetting("drag_modifier", drag_modifier_);
    LoadSetting("move_modifier", move_modifier_);
    opacity_ = std::clamp(opacity_, 0, 100);
    zoom_ = std::isfinite(zoom_) ? std::clamp(zoom_, .15f, 8.f) : 1.f;
    map_modifier_ = std::clamp(map_modifier_, 0, 3);
    target_modifier_ = std::clamp(target_modifier_, 0, 3);
    drag_modifier_ = std::clamp(drag_modifier_, 0, 3);
    move_modifier_ = std::clamp(move_modifier_, 0, 3);
}

void TacticalMinimap::SaveSettings(const wchar_t* folder)
{
    std::scoped_lock lock(mutex_);
    SaveSetting("tactical_mode", tactical_);
    SaveSetting("opacity", opacity_);
    SaveSetting("rotate", rotate_);
    SaveSetting("ranges", show_ranges_);
    SaveSetting("zoom", zoom_);
    SaveSetting("map_modifier", map_modifier_);
    SaveSetting("target_modifier", target_modifier_);
    SaveSetting("drag_modifier", drag_modifier_);
    SaveSetting("move_modifier", move_modifier_);
    ToolboxUIPlugin::SaveSettings(folder);
}
