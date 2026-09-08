#include "PitsSoulsWindow.h"
#include <AsyncStringDecoder.h>

#include <GWCA/Constants/Constants.h>
#include <GWCA/GameContainers/Array.h>
#include <GWCA/GameEntities/Agent.h>
#include <GWCA/Managers/AgentMgr.h>
#include <GWCA/Managers/MapMgr.h>
#include <GWCA/Managers/StoCMgr.h>
#include <GWCA/Packets/StoC.h>

#include <cmath>

#ifndef DBBOX_BUILD
DLLAPI ToolboxPlugin* ToolboxPluginInstance()
{
    static PitsSoulsWindow instance;
    return &instance;
}
#endif

namespace {
    constexpr auto SoulLifetimeSeconds = 103.6;
    constexpr auto SoulRespawnSeconds = 120.0;
    // The sites are over 1,700 units apart; tolerate movement without mixing their timers.
    constexpr auto SoulMatchRadius = 500.f;

    void SetNextWindowCenter(const ImGuiWindowFlags flags)
    {
        const auto& io = ImGui::GetIO();
        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), flags, ImVec2(0.5f, 0.5f));
    }
}

std::string PitsSoulsWindow::PitsSoul::print() const
{
    switch (currentState) {
        case State::Alive:
            return std::string(name) + ": Death in " + std::to_string(static_cast<int>(secondsRemaining)) + "s";
        case State::Dead:
            return std::string(name) + ": Respawn in " + std::to_string(static_cast<int>(secondsRemaining)) + "s";
        default:
            return std::string(name) + (agent_id ? ": Unknown (waiting for health)" : ": Unknown (soul not found)");
    }
}

void PitsSoulsWindow::PitsSoul::observeDeath(const Clock::time_point now)
{
    if (!observedDead || !expectedDeathTime) expectedDeathTime = now;
    observedDead = true;
    readHpPercent.reset();
}

void PitsSoulsWindow::PitsSoul::observe(const float hp, const bool dead, const Clock::time_point now)
{
    if (dead) {
        if (!observedDead && expectedDeathTime) observeDeath(now);
        observedDead = true;
        return;
    }
    if (!std::isfinite(hp) || hp <= 0.f || hp > 1.f) return;

    // An unchanged client health cache is not a fresh sample of the soul's degeneration.
    if (observedDead || !readHpPercent || *readHpPercent != hp) {
        expectedDeathTime = now + std::chrono::duration_cast<Clock::duration>(
            std::chrono::duration<double>(SoulLifetimeSeconds * hp));
        readHpPercent = hp;
    }
    observedDead = false;
}

void PitsSoulsWindow::PitsSoul::update(const Clock::time_point now)
{
    if (!expectedDeathTime) return;
    const auto secondsSinceDeath = std::chrono::duration<double>(now - *expectedDeathTime).count();
    if (secondsSinceDeath < 0) {
        currentState = State::Alive;
        secondsRemaining = -secondsSinceDeath;
        return;
    }

    // Keep the original phase when updates pause or the souls leave compass range.
    const auto phase = std::fmod(secondsSinceDeath, SoulRespawnSeconds + SoulLifetimeSeconds);
    if (phase < SoulRespawnSeconds) {
        currentState = State::Dead;
        secondsRemaining = SoulRespawnSeconds - phase;
    }
    else {
        currentState = State::Alive;
        secondsRemaining = SoulRespawnSeconds + SoulLifetimeSeconds - phase;
    }
}

bool PitsSoulsWindow::isChainedSoul(const GW::AgentLiving& agent)
{
    const auto encoded = GW::Agents::GetAgentEncName(agent.agent_id);
    if (!encoded || !*encoded) return false;
    const auto key = std::wstring(encoded);
    auto found = decoded_names.find(key);
    if (found != decoded_names.end() && found->second->result == DecodedName::Result::Failed
        && Clock::now() >= found->second->retry_after) {
        decoded_names.erase(found);
        found = decoded_names.end();
    }
    if (found == decoded_names.end()) {
        constexpr auto maximum_cached_names = size_t{128};
        if (decoded_names.size() >= maximum_cached_names) {
            std::erase_if(decoded_names, [](const auto& entry) { return entry.second->result != DecodedName::Result::Pending; });
        }
        if (decoded_names.size() >= maximum_cached_names) return false;
        const auto state = std::make_shared<DecodedName>();
        found = decoded_names.emplace(key, state).first;
        // Only owned decode state survives a map change or unload; no agent or plugin pointers escape.
        AsyncStringDecoder::Decode(key, [state](const wchar_t* decoded) {
            state->result = !decoded || !*decoded ? DecodedName::Result::Failed
                : _wcsicmp(decoded, L"Chained Soul") == 0 ? DecodedName::Result::ChainedSoul : DecodedName::Result::Other;
        }, GW::Constants::Language::English);
    }
    return found->second->result == DecodedName::Result::ChainedSoul;
}

void PitsSoulsWindow::resetSouls()
{
    decoded_names.clear();
    souls = {{
        {.pos = {11427.f, 5079.f}, .name = "Bottom"},
        {.pos = {9245.f, 4898.f}, .name = "Double"},
        {.pos = {10130.f, 6616.f}, .name = "Reaper"}
    }};
    last_instance_time = 0;
    underworld_instance = false;
}

PitsSoulsWindow::PitsSoul* PitsSoulsWindow::findSoul(const GW::Vec2f pos)
{
    auto nearest = static_cast<PitsSoul*>(nullptr);
    auto distance = SoulMatchRadius * SoulMatchRadius;
    for (auto& soul : souls) {
        const auto candidate = GW::GetSquareDistance(pos, soul.pos);
        if (std::isfinite(candidate) && candidate <= distance) {
            nearest = &soul;
            distance = candidate;
        }
    }
    return nearest;
}

void PitsSoulsWindow::bindSoul(PitsSoul& soul, const uint32_t agent_id)
{
    if (soul.agent_id == agent_id) return;
    soul.agent_id = agent_id;
    soul.readHpPercent.reset();
    soul.observedDead = false;
}

void PitsSoulsWindow::Update(const float delay)
{
    ToolboxUIPlugin::Update(delay);
    const std::scoped_lock lock(state_mutex);
    if (terminating) return;
    const auto instance_type = GW::Map::GetInstanceType();
    if (instance_type == GW::Constants::InstanceType::Loading) return;
    if (GW::Map::GetMapID() != GW::Constants::MapID::The_Underworld
        || instance_type != GW::Constants::InstanceType::Explorable || GW::Agents::IsObserving()) {
        resetSouls();
        return;
    }
    if (!underworld_instance || !GW::Agents::GetControlledCharacter()) return;
    const auto instance_time = GW::Map::GetInstanceTime();
    if (instance_time < last_instance_time) {
        resetSouls();
        underworld_instance = true;
    }
    last_instance_time = instance_time;
    const auto now = Clock::now();

    std::array<const GW::AgentLiving*, 3> candidates{};
    const auto agents = GW::Agents::GetAgentArray();
    const auto valid_agents = agents && agents->valid() && agents->m_buffer;
    if (valid_agents) {
        for (const auto agent : *agents) {
            const auto living = agent ? agent->GetAsAgentLiving() : nullptr;
            if (!living || living->IsPlayer()) continue;
            const auto soul = findSoul(living->pos);
            if (!soul || !isChainedSoul(*living)) continue;
            auto& candidate = candidates[static_cast<size_t>(soul - souls.data())];
            if (!candidate || (candidate->GetIsDead() && !living->GetIsDead())
                || (candidate->GetIsDead() == living->GetIsDead()
                    && GW::GetSquareDistance(living->pos, soul->pos) < GW::GetSquareDistance(candidate->pos, soul->pos))) {
                candidate = living;
            }
        }
    }

    const auto map_agents = GW::Agents::GetMapAgentArray();
    const auto valid_map_agents = map_agents && map_agents->valid() && map_agents->m_buffer;
    for (auto i = size_t{0}; i < souls.size(); ++i) {
        auto& soul = souls[i];
        const auto bound_entry = soul.agent_id && valid_agents ? agents->get(soul.agent_id) : nullptr;
        if (bound_entry && *bound_entry) {
            const auto bound = (*bound_entry)->GetAsAgentLiving();
            if (!bound || bound->IsPlayer() || !isChainedSoul(*bound))
                bindSoul(soul, 0);
        }
        const auto living = candidates[i];
        if (living) bindSoul(soul, living->agent_id);
        auto hp = living ? living->hp : 0.f;
        auto dead = living && living->GetIsDead();
        const auto map_agent = soul.agent_id && valid_map_agents ? map_agents->get(soul.agent_id) : nullptr;
        if (!dead && (!std::isfinite(hp) || hp <= 0.f || hp > 1.f) && map_agent) {
            dead = map_agent->GetIsDead();
            if (std::isfinite(map_agent->cur_health) && std::isfinite(map_agent->max_health)
                && map_agent->cur_health > 0.f && map_agent->max_health > 0.f && map_agent->cur_health <= map_agent->max_health)
                hp = map_agent->cur_health / map_agent->max_health;
        }
        soul.observe(hp, dead, now);
        soul.update(now);
    }
}

void PitsSoulsWindow::Draw(IDirect3DDevice9* pDevice)
{
    UNREFERENCED_PARAMETER(pDevice);
    const std::scoped_lock lock(state_mutex);
    if (terminating || !GetVisiblePtr() || !*GetVisiblePtr()) return;

    SetNextWindowCenter(ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(100, 0), ImGuiCond_FirstUseEver);
    if (ImGui::Begin(Name(), GetVisiblePtr(), GetWinFlags())) {
        if (!underworld_instance) {
            ImGui::TextUnformatted("Waiting for the Underworld");
        }
        else {
            for (const auto& soul : souls) ImGui::Text("%s", soul.print().c_str());
        }
    }
    ImGui::End();
}

void PitsSoulsWindow::DrawSettings()
{
    ToolboxUIPlugin::DrawSettings();

    ImGui::Text("Version 1.1.5");
    ImGui::TextUnformatted("Tracks the Chained Souls near Bottom, Double and Reaper in the Underworld.");
    ImGui::TextUnformatted("Move within compass range to acquire a soul; estimates continue after leaving range.");
}

void PitsSoulsWindow::Initialize(ImGuiContext* ctx, ImGuiAllocFns fns, HMODULE toolbox_dll)
{
    ToolboxUIPlugin::Initialize(ctx, fns, toolbox_dll);
    {
        const std::scoped_lock lock(state_mutex);
        terminating = false;
        resetSouls();
        underworld_instance = GW::Map::GetMapID() == GW::Constants::MapID::The_Underworld
            && GW::Map::GetInstanceType() == GW::Constants::InstanceType::Explorable && !GW::Agents::IsObserving();
    }
    GW::StoC::RegisterPacketCallback<GW::Packet::StoC::GameSrvTransfer>(
        &transfer_hook, [this](GW::HookStatus*, const auto*) {
            const std::scoped_lock lock(state_mutex);
            resetSouls();
        });
    GW::StoC::RegisterPacketCallback<GW::Packet::StoC::InstanceLoadInfo>(
        &instance_hook, [this](GW::HookStatus*, const auto* packet) {
            const std::scoped_lock lock(state_mutex);
            resetSouls();
            underworld_instance = !terminating && packet && packet->is_explorable && !packet->is_observer
                && packet->map_id == static_cast<uint32_t>(GW::Constants::MapID::The_Underworld);
        }, 0x8000);
    GW::StoC::RegisterPacketCallback<GW::Packet::StoC::AgentAdd>(
        &spawn_hook, [this](GW::HookStatus*, const auto* packet) {
            const std::scoped_lock lock(state_mutex);
            if (terminating || !underworld_instance || !packet || !packet->agent_id) return;
            const auto soul = findSoul(packet->position);
            if (!soul) return;
            const auto agent = GW::Agents::GetAgentByID(packet->agent_id);
            const auto living = agent ? agent->GetAsAgentLiving() : nullptr;
            if (living && !living->IsPlayer() && isChainedSoul(*living)) bindSoul(*soul, living->agent_id);
        }, 0x8000);
    GW::StoC::RegisterPacketCallback<GW::Packet::StoC::AgentRemove>(
        &despawn_hook, [this](GW::HookStatus*, const auto* packet) {
            const std::scoped_lock lock(state_mutex);
            if (!packet) return;
            for (auto& soul : souls) {
                if (soul.agent_id == packet->agent_id) bindSoul(soul, 0);
            }
        });
    GW::StoC::RegisterPacketCallback<GW::Packet::StoC::AgentState>(
        &state_hook, [this](GW::HookStatus*, const auto* packet) {
            const std::scoped_lock lock(state_mutex);
            if (terminating || !underworld_instance || !packet) return;
            const auto now = Clock::now();
            for (auto& soul : souls) {
                if (soul.agent_id && soul.agent_id == packet->agent_id) {
                    if (packet->state & 0x10) {
                        soul.observeDeath(now);
                    }
                    else if (soul.observedDead) {
                        soul.observedDead = false;
                        soul.readHpPercent.reset();
                    }
                    soul.update(now);
                }
            }
        }, 0x8000);
}

void PitsSoulsWindow::SignalTerminate()
{
    {
        const std::scoped_lock lock(state_mutex);
        terminating = true;
    }
    GW::StoC::RemoveCallbacks(&transfer_hook);
    GW::StoC::RemoveCallbacks(&instance_hook);
    GW::StoC::RemoveCallbacks(&spawn_hook);
    GW::StoC::RemoveCallbacks(&despawn_hook);
    GW::StoC::RemoveCallbacks(&state_hook);
    {
        const std::scoped_lock lock(state_mutex);
        resetSouls();
    }
    ToolboxUIPlugin::SignalTerminate();
}

bool PitsSoulsWindow::CanTerminate()
{
    return AsyncStringDecoder::PendingCount() == 0 && ToolboxUIPlugin::CanTerminate();
}
