#include "DhuumCalculator.h"

#include <GWCA/Constants/Constants.h>
#include <GWCA/GameEntities/Agent.h>
#include <GWCA/GWCA.h>

#include <GWCA/Managers/AgentMgr.h>
#include <GWCA/Managers/MapMgr.h>
#include <GWCA/Managers/StoCMgr.h>
#include <GWCA/Packets/StoC.h>


#include <cmath>
#include <limits>
#include <numeric>

#ifndef DBBOX_BUILD
DLLAPI ToolboxPlugin* ToolboxPluginInstance()
{
    static DhuumCalculator instance;
    return &instance;
}
#endif

namespace
{
    const GW::AgentLiving* findDhuum() 
    {
        const auto agents = GW::Agents::GetAgentArray();
        if (!agents || !agents->valid() || (agents->size() && !agents->m_buffer)) return nullptr;
        for (const auto* agent : *agents) {
            if (!agent || !agent->GetIsLivingType()) 
                continue;
            const auto living = agent->GetAsAgentLiving();
            if (!living->IsPlayer() && !living->GetIsDead() && living->player_number == GW::Constants::ModelID::UW::Dhuum)
                return living;
        }
        return nullptr;
    }

    int64_t diff(const std::chrono::steady_clock::time_point& a, const std::chrono::steady_clock::time_point& b) 
    {
        return int64_t(std::chrono::duration_cast<std::chrono::milliseconds>(a - b).count());
    }

    template<size_t smoothingLevel, typename T>
    T smooth(T value, std::deque<T>& queue) 
    {
        queue.push_back(value);
        if (queue.size() > smoothingLevel) queue.pop_front();
        return std::accumulate(queue.begin(), queue.end(), T{0}, std::plus<T>()) / static_cast<T>(queue.size());
    }

    std::string formatTime(int64_t timeInMs) 
    {
        const auto minutes = timeInMs / 60'000u;
        const auto seconds = (timeInMs % 60'000u) / 1'000u;
        return std::to_string(minutes) + ":" + (seconds < 10 ? "0" : "") + std::to_string(seconds);
    }
} // namespace

void DhuumCalculator::resetPredictions()
{
    history.clear();
    damagePredictions.clear();
    restPredictions.clear();
    missingDamagePredictions.clear();
    damageFinishPrediction = 0;
    restFinishPrediction = 0;
    missingDamagePrediction = 0;
    predictions_ready = false;
}

void DhuumCalculator::resetEncounter()
{
    dhuum_agent_id = 0;
    has_dhuum = false;
    underworld_instance = false;
    last_instance_time = 0;
    next_agent_search = {};
    progress_id.reset();
    rest_progress.reset();
    resetPredictions();
}

void DhuumCalculator::OnMissionProgress(const uint8_t id, const float filled, const bool created)
{
    const std::scoped_lock lock(state_mutex);
    if (terminating || !underworld_instance) return;
    if (!created && progress_id && *progress_id != id) return;
    if (created) resetPredictions();
    progress_id = id;
    if (!std::isfinite(filled) || filled < 0.f || filled > 1.f) {
        rest_progress.reset();
        resetPredictions();
        return;
    }
    rest_progress = filled;
}

void DhuumCalculator::Initialize(ImGuiContext* ctx, ImGuiAllocFns allocator_fns, HMODULE toolbox_dll)
{
    ToolboxUIPlugin::Initialize(ctx, allocator_fns, toolbox_dll);
    {
        const std::scoped_lock lock(state_mutex);
        terminating = false;
        resetEncounter();
        underworld_instance = GW::Map::GetMapID() == GW::Constants::MapID::The_Underworld
            && GW::Map::GetInstanceType() == GW::Constants::InstanceType::Explorable
            && !GW::Agents::IsObserving();
    }
    GW::StoC::RegisterPacketCallback<GW::Packet::StoC::GameSrvTransfer>(
        &transfer_hook, [this](GW::HookStatus*, const auto*) {
            const std::scoped_lock lock(state_mutex);
            resetEncounter();
        });
    GW::StoC::RegisterPacketCallback<GW::Packet::StoC::InstanceLoadInfo>(
        &instance_hook, [this](GW::HookStatus*, const auto* packet) {
            const std::scoped_lock lock(state_mutex);
            resetEncounter();
            underworld_instance = !terminating && packet && packet->is_explorable && !packet->is_observer
                && packet->map_id == static_cast<uint32_t>(GW::Constants::MapID::The_Underworld);
        }, 0x8000);
    GW::StoC::RegisterPacketCallback<GW::Packet::StoC::AgentAdd>(
        &agent_add_hook, [this](GW::HookStatus*, const auto* packet) {
            const std::scoped_lock lock(state_mutex);
            if (!terminating && packet && (packet->agent_type & 0xF0000000) == 0x20000000
                && (packet->agent_type & 0xFFFFFF) == GW::Constants::ModelID::UW::Dhuum)
                next_agent_search = {};
        }, 0x8000);
    GW::StoC::RegisterPacketCallback<GW::Packet::StoC::AgentRemove>(
        &agent_remove_hook, [this](GW::HookStatus*, const auto* packet) {
            const std::scoped_lock lock(state_mutex);
            if (packet && packet->agent_id == dhuum_agent_id) {
                dhuum_agent_id = 0;
                has_dhuum = false;
                next_agent_search = {};
                resetPredictions();
            }
        });
    GW::StoC::RegisterPacketCallback<GW::Packet::StoC::CreateMissionProgress>(
        &progress_create_hook, [this](GW::HookStatus*, const auto* packet) {
            if (packet) OnMissionProgress(packet->id, packet->filled, true);
        }, 0x8000);
    GW::StoC::RegisterPacketCallback<GW::Packet::StoC::UpdateMissionProgress>(
        &progress_update_hook, [this](GW::HookStatus*, const auto* packet) {
            if (packet) OnMissionProgress(packet->id, packet->filled, false);
        }, 0x8000);
}

void DhuumCalculator::SignalTerminate()
{
    {
        const std::scoped_lock lock(state_mutex);
        terminating = true;
    }
    GW::StoC::RemoveCallbacks(&transfer_hook);
    GW::StoC::RemoveCallbacks(&instance_hook);
    GW::StoC::RemoveCallbacks(&agent_add_hook);
    GW::StoC::RemoveCallbacks(&agent_remove_hook);
    GW::StoC::RemoveCallbacks(&progress_create_hook);
    GW::StoC::RemoveCallbacks(&progress_update_hook);
    {
        const std::scoped_lock lock(state_mutex);
        resetEncounter();
    }
    ToolboxUIPlugin::SignalTerminate();
}

void DhuumCalculator::Update(float delay)
{
    ToolboxUIPlugin::Update(delay);
    const std::scoped_lock lock(state_mutex);
    if (terminating) return;
    const auto instance_type = GW::Map::GetInstanceType();
    const auto map_id = GW::Map::GetMapID();
    if (instance_type != GW::Constants::InstanceType::Loading
        && (instance_type != GW::Constants::InstanceType::Explorable || map_id != GW::Constants::MapID::The_Underworld)) {
        resetEncounter();
        return;
    }
    if (!underworld_instance || map_id != GW::Constants::MapID::The_Underworld
        || instance_type != GW::Constants::InstanceType::Explorable
        || !GW::Agents::GetControlledCharacter()) {
        has_dhuum = false;
        dhuum_agent_id = 0;
        resetPredictions();
        return;
    }

    const auto instanceTime = GW::Map::GetInstanceTime();
    if (instanceTime < last_instance_time) {
        resetEncounter();
        underworld_instance = true;
    }
    last_instance_time = instanceTime;
    const auto now = std::chrono::steady_clock::now();
    const auto agent = dhuum_agent_id ? GW::Agents::GetAgentByID(dhuum_agent_id) : nullptr;
    const auto* dhuum = agent ? agent->GetAsAgentLiving() : nullptr;
    if (dhuum && (dhuum->IsPlayer() || dhuum->GetIsDead()
        || dhuum->player_number != GW::Constants::ModelID::UW::Dhuum))
        dhuum = nullptr;
    if (!dhuum && now >= next_agent_search) {
        dhuum = findDhuum();
        next_agent_search = now + std::chrono::seconds(1);
    }
    const auto found_id = dhuum ? dhuum->agent_id : 0;
    if (found_id != dhuum_agent_id) resetPredictions();
    dhuum_agent_id = found_id;
    has_dhuum = dhuum && std::isfinite(dhuum->hp) && dhuum->hp >= 0.f && dhuum->hp <= 1.f
        && dhuum->max_hp > 0;
    if (!has_dhuum) {
        resetPredictions();
        return;
    }

    // Draw only consumes values; an agent may leave memory between Update and Draw.
    health = dhuum->hp;
    max_health = dhuum->max_hp;
    if (!rest_progress) return;
    if (!history.empty() && diff(now, history.back().time) < 250) return;
    history.push_back({std::max(0.f, health - 0.25f), 1.f - *rest_progress, now});
    while (!history.empty() && diff(now, history.front().time) > 48'000) {
        history.pop_front();
    }

    predictions_ready = false;
    if (history.size() < 2) return;
    const auto elapsed = diff(history.back().time, history.front().time);
    if (elapsed <= 0) return;
    const auto dpms = (history.back().hp - history.front().hp) / elapsed;
    const auto rpms = (history.back().rest - history.front().rest) / elapsed;
    constexpr auto minimumRate = std::numeric_limits<float>::epsilon();
    if (!std::isfinite(dpms) || !std::isfinite(rpms) || std::abs(dpms) <= minimumRate || std::abs(rpms) <= minimumRate) return;

    const auto damage_eta = std::abs(static_cast<double>(history.back().hp) / dpms);
    const auto rest_eta = std::abs(static_cast<double>(history.back().rest) / rpms);
    const auto missing_damage = (0.75 * max_health) * (history.back().hp - std::abs(rest_eta * dpms));
    constexpr auto maximumPrediction = static_cast<double>(std::numeric_limits<int64_t>::max() / 32);
    if (!std::isfinite(damage_eta) || !std::isfinite(rest_eta) || !std::isfinite(missing_damage)
        || damage_eta > maximumPrediction || rest_eta > maximumPrediction || std::abs(missing_damage) > maximumPrediction)
        return;

    damageFinishPrediction = smooth<16>(static_cast<int64_t>(damage_eta) + instanceTime, damagePredictions);
    restFinishPrediction = smooth<16>(static_cast<int64_t>(rest_eta) + instanceTime, restPredictions);
    missingDamagePrediction = smooth<32>(static_cast<int64_t>(missing_damage), missingDamagePredictions);
    predictions_ready = true;
}

void DhuumCalculator::Draw(IDirect3DDevice9* pDevice)
{
    UNREFERENCED_PARAMETER(pDevice);
    const std::scoped_lock lock(state_mutex);
    const auto visible = GetVisiblePtr();
    if (terminating || !underworld_instance || !visible || !*visible) return;

    ImGui::SetNextWindowSize(ImVec2(100, 0), ImGuiCond_FirstUseEver);
    if (ImGui::Begin(Name(), visible, GetWinFlags()))
    {
        if (!has_dhuum) {
            ImGui::TextUnformatted("Dhuum not found");
        }
        else if (!rest_progress) {
            ImGui::TextUnformatted("Waiting for Dhuum's Rest update");
        }
        else if (health <= 0.25f) {
            const auto missingRests = static_cast<int>(std::ceil((1.f - *rest_progress) / 0.1f));
            ImGui::Text("Damage done. Missing rests: %i", missingRests);
        }
        else if (*rest_progress >= 0.999f) {
            const auto missingFuries = std::ceil((static_cast<double>(health) - 0.25) * max_health / 250);
            ImGui::Text("Rest done. Missing furies: %.0f", missingFuries);
        }
        else if (!predictions_ready) {
            ImGui::TextUnformatted("Calculating finish times...");
        }
        else {
            ImGui::Text("Rest finish time: %s", formatTime(restFinishPrediction).c_str());
            ImGui::Text("Damage finish time: %s", formatTime(damageFinishPrediction).c_str());
            if (missingDamagePrediction > 0) {
                ImGui::Text("Missing furies: %lld", missingDamagePrediction / 250);
            }
            else {
                ImGui::Text("Overkilling by: %lldk", -missingDamagePrediction / 1000);
            }
        }
    }
    ImGui::End();
}

void DhuumCalculator::DrawSettings()
{
    ToolboxUIPlugin::DrawSettings();

    ImGui::Text("Version 1.0.4");
}
