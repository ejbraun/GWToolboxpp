#include "PartyLogPlugin.h"

#include <Path.h> // Core: PathGetDocumentsPath / PathGetComputerName

#include <GWCA/Constants/Constants.h>
#include <GWCA/Context/CharContext.h>
#include <GWCA/GameEntities/Agent.h>
#include <GWCA/GameEntities/Party.h>
#include <GWCA/GameEntities/Player.h>
#include <GWCA/Managers/AgentMgr.h>
#include <GWCA/Managers/PartyMgr.h>
#include <GWCA/Managers/PlayerMgr.h>
#include <GWCA/Managers/StoCMgr.h>
// GWCA/Constants/UIMessages.h has no include guard - don't include it directly. UIMgr.h (which does
// have one) already pulls it in internally; including both causes its content to be pasted twice in
// this TU, which corrupts parsing for the rest of that file and shows up as bogus "undeclared
// identifier"/"undefined type" errors for symbols defined later in it.
#include <GWCA/Managers/UIMgr.h>
#include <GWCA/Packets/StoC.h>

#include <glaze/glaze.hpp>

#include <filesystem>
#include <fstream>
#include <sstream>

// Mirrors the shape written to disk; kept separate from the live PartyLogPlugin::PartyMember only in
// name, not in fields. Needs external linkage (i.e. can't live in an anonymous namespace) — glaze's
// reflection generates a stable name per type and errors (C7631) on internal-linkage types.
struct LogEntry {
    uint32_t utc_start = 0;
    uint32_t map_id = 0;
    std::string character_name;
    std::string end_reason; // "wipe", "resign", or "unknown"
    std::vector<PartyLogPlugin::PartyMember> party_members;
};

namespace {
    std::filesystem::path GetRunsFolder()
    {
        std::filesystem::path computer_name;
        std::filesystem::path docs;
        if (!PathGetComputerName(computer_name) || !PathGetDocumentsPath(docs, L"GWToolboxpp")) {
            return {};
        }
        return docs / computer_name / L"runs";
    }

    // Same folder GWToolboxdll's Objective Timer writes ObjectiveTimerRuns_*.json into, so a single
    // cron job can watch one directory. Day bucket uses UTC, matching how GWToolboxdll buckets its own files.
    std::filesystem::path GetLogFilePath(const uint32_t utc_start)
    {
        const auto folder = GetRunsFolder();
        if (folder.empty()) {
            return {};
        }
        const time_t tt = utc_start;
        const tm* timeinfo = gmtime(&tt);
        if (!timeinfo) {
            return {};
        }
        wchar_t filename[40];
        swprintf(filename, _countof(filename), L"PartyLog_%04d-%02d-%02d.json",
                 timeinfo->tm_year + 1900, timeinfo->tm_mon + 1, timeinfo->tm_mday);
        return folder / filename;
    }

    // Encoded prefix for the "<player> has resigned." system chat message. Not human-readable text -
    // it's GW's fixed per-template control-code sequence, so it matches regardless of the client's
    // display language (only the decoded text varies by language, not the encoded template id). Same
    // bytes GWToolboxdll's own ResignLogModule matches on.
    constexpr wchar_t kResignedPrefix[] = L"\x7BFF\xC9C4\xAEAA\x1B9B\x107";
}

DLLAPI ToolboxPlugin* ToolboxPluginInstance()
{
    static PartyLogPlugin instance;
    return &instance;
}

void PartyLogPlugin::Initialize(ImGuiContext* ctx, const ImGuiAllocFns allocator_fns, const HMODULE toolbox_dll)
{
    ToolboxPlugin::Initialize(ctx, allocator_fns, toolbox_dll);
    // Positive altitude: triggered after the packet has been processed by the game/GWCA.
    GW::StoC::RegisterPacketCallback<GW::Packet::StoC::InstanceLoadInfo>(
        &InstanceLoadInfo_HookEntry,
        [this](GW::HookStatus*, const GW::Packet::StoC::InstanceLoadInfo* packet) {
            OnInstanceLoadInfo(packet->map_id, packet->is_explorable != 0);
        },
        1);
    GW::StoC::RegisterPacketCallback<GW::Packet::StoC::GameSrvTransfer>(
        &GameSrvTransfer_HookEntry,
        [this](GW::HookStatus*, GW::Packet::StoC::GameSrvTransfer*) { OnGameSrvTransfer(); });
    GW::StoC::RegisterPacketCallback<GW::Packet::StoC::PartyDefeated>(
        &PartyDefeated_HookEntry,
        [this](GW::HookStatus*, GW::Packet::StoC::PartyDefeated*) { OnPartyDefeated(); });
    GW::UI::RegisterUIMessageCallback(
        &WriteToChatLog_HookEntry, GW::UI::UIMessage::kWriteToChatLog,
        [this](GW::HookStatus*, GW::UI::UIMessage, void* wParam, void*) {
            OnWriteToChatLog(static_cast<GW::UI::UIPacket::kWriteToChatLog*>(wParam)->message);
        },
        0x8000);
}

void PartyLogPlugin::Terminate()
{
    GW::UI::RemoveUIMessageCallback(&WriteToChatLog_HookEntry, GW::UI::UIMessage::kWriteToChatLog);
    GW::StoC::RemoveCallback<GW::Packet::StoC::PartyDefeated>(&PartyDefeated_HookEntry);
    GW::StoC::RemoveCallback<GW::Packet::StoC::GameSrvTransfer>(&GameSrvTransfer_HookEntry);
    GW::StoC::RemoveCallback<GW::Packet::StoC::InstanceLoadInfo>(&InstanceLoadInfo_HookEntry);
    ToolboxPlugin::Terminate();
}

void PartyLogPlugin::OnInstanceLoadInfo(const uint32_t map_id, const bool is_explorable)
{
    if (!is_explorable) {
        return;
    }
    next_utc_start = static_cast<uint32_t>(time(nullptr));
    next_map_id = map_id;
    next_character_name.clear();
    if (const GW::CharContext* cc = GW::GetCharContext()) {
        next_character_name = PluginUtils::WStringToString(cc->player_name);
    }
    restart_requested = true;

    run_active = true;
    wipe_detected = false;
    resigned_login_numbers.clear();
}

void PartyLogPlugin::OnPartyDefeated()
{
    if (!run_active) {
        return;
    }
    wipe_detected = true;
}

void PartyLogPlugin::OnWriteToChatLog(const wchar_t* message)
{
    if (!run_active || !message || wmemcmp(message, kResignedPrefix, 5) != 0) {
        return;
    }
    const std::wstring resigned_name = PluginUtils::GetPlayerNameFromEncodedString(message);
    if (resigned_name.empty()) {
        return;
    }
    const GW::PartyInfo* info = GW::PartyMgr::GetPartyInfo();
    if (!info) {
        return;
    }
    for (const auto& player : info->players) {
        const wchar_t* name_ptr = GW::PlayerMgr::GetPlayerName(player.login_number);
        if (!name_ptr) {
            continue;
        }
        if (PluginUtils::SanitizePlayerName(name_ptr) == resigned_name) {
            resigned_login_numbers.insert(player.login_number);
            return;
        }
    }
}

void PartyLogPlugin::OnGameSrvTransfer()
{
    if (!run_active) {
        return;
    }
    run_active = false;

    if (restart_requested || active_capture || party_members.empty()) {
        return; // party capture never completed for this run; nothing worth logging
    }

    // Checked before wipe_detected: resign is the more specific signal (every connected player
    // individually confirmed via their own "has resigned" chat message), whereas PartyDefeated
    // appears to also fire when the whole party resigns, not just on an actual death-wipe.
    std::string end_reason = "unknown";
    if (const GW::PartyInfo* info = GW::PartyMgr::GetPartyInfo()) {
        bool any_connected = false;
        bool all_resigned = true;
        for (const auto& player : info->players) {
            if (!player.connected()) {
                continue;
            }
            any_connected = true;
            if (!resigned_login_numbers.contains(player.login_number)) {
                all_resigned = false;
                break;
            }
        }
        if (any_connected && all_resigned) {
            end_reason = "resign";
        }
    }
    if (end_reason == "unknown" && wipe_detected) {
        end_reason = "wipe";
    }
    WriteLogEntry(end_reason);
}

void PartyLogPlugin::Update(float)
{
    CaptureParty();
}

void PartyLogPlugin::CaptureParty()
{
    if (restart_requested) {
        // Only safe to tear down the previous run's EncStrings once none are still mid-decode -
        // destroying one while GW::UI::AsyncDecodeStr's callback is still pending is a use-after-free.
        for (const auto& enc : party_member_enc_names) {
            if (enc->IsDecoding()) {
                return; // let the old capture's decodes finish before starting the new one
            }
        }
        party_members.clear();
        party_member_enc_names.clear();
        pending_utc_start = next_utc_start;
        pending_map_id = next_map_id;
        pending_character_name = next_character_name;
        restart_requested = false;
        active_capture = true;
    }

    if (!active_capture) {
        return;
    }

    if (!party_member_enc_names.empty()) {
        // Waiting on names queued last pass to finish decoding.
        for (const auto& enc : party_member_enc_names) {
            if (enc->IsDecoding()) {
                return;
            }
        }
        for (size_t i = 0; i < party_members.size(); i++) {
            party_members[i].name = party_member_enc_names[i]->string();
        }
        party_member_enc_names.clear();
        active_capture = false;
        return; // logged at run end (OnGameSrvTransfer), once the outcome is known
    }

    const GW::PartyInfo* info = GW::PartyMgr::GetPartyInfo();
    if (!info) {
        return; // not yet available; retry next tick
    }

    const auto add_member = [&](const uint32_t agent_id, const wchar_t* enc_name,
                                 const bool is_player, const bool is_hero, const bool is_henchman) {
        uint32_t primary = 0;
        uint32_t secondary = 0;
        if (const GW::Agent* agent = GW::Agents::GetAgentByID(agent_id)) {
            if (const GW::AgentLiving* living = agent->GetAsAgentLiving()) {
                primary = static_cast<uint32_t>(living->primary);
                secondary = static_cast<uint32_t>(living->secondary);
            }
        }
        party_members.push_back(PartyLogPlugin::PartyMember{
            .primary = primary,
            .secondary = secondary,
            .is_player = is_player,
            .is_hero = is_hero,
            .is_henchman = is_henchman
        });
        // NB: Player may have left the game, meaning GW::Agents::GetAgentEncName(agent_id) would fail
        // because the agent is gone. Pass enc_name for real players instead.
        auto enc = std::make_unique<PluginUtils::EncString>();
        enc->reset(enc_name ? enc_name : GW::Agents::GetAgentEncName(agent_id));
        enc->wstring(); // trigger decode
        party_member_enc_names.push_back(std::move(enc));
    };

    for (const auto& player : info->players) {
        if (const GW::Player* gwplayer = GW::PlayerMgr::GetPlayerByID(player.login_number)) {
            add_member(gwplayer->agent_id, gwplayer->name_enc, true, false, false);
        }
    }
    for (const auto& hero : info->heroes) {
        add_member(hero.agent_id, nullptr, false, true, false);
    }
    for (const auto& hench : info->henchmen) {
        add_member(hench.agent_id, nullptr, false, false, true);
    }

    if (party_members.empty()) {
        active_capture = false; // no party info found; nothing to log
    }
}

void PartyLogPlugin::WriteLogEntry(const std::string& end_reason)
{
    if (party_members.empty()) {
        return;
    }
    const auto path = GetLogFilePath(pending_utc_start);
    if (path.empty()) {
        return;
    }

    try {
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);

        std::vector<LogEntry> entries;
        if (std::ifstream in{path}; in.is_open()) {
            std::stringstream ss;
            ss << in.rdbuf();
            constexpr glz::opts opts{.error_on_unknown_keys = false};
            if (const auto read_ec = glz::read<opts>(entries, ss.str()); read_ec) {
                entries.clear(); // don't trust partially-parsed data on error
            }
        }

        // Replace any earlier entry for the same run (e.g. a district hop re-firing InstanceLoadInfo).
        std::erase_if(entries, [&](const LogEntry& e) {
            return e.utc_start == pending_utc_start && e.character_name == pending_character_name;
        });
        entries.push_back(LogEntry{
            .utc_start = pending_utc_start,
            .map_id = pending_map_id,
            .character_name = pending_character_name,
            .end_reason = end_reason,
            .party_members = party_members,
        });

        std::ofstream out{path};
        if (out.is_open()) {
            out << glz::write_json(entries).value_or(std::string{});
            last_written_utc_start = pending_utc_start;
        }
    } catch (const std::exception&) {
        // Best-effort logging; nothing to do if the runs folder is unwritable.
    }
}

void PartyLogPlugin::DrawSettings()
{
    ImGui::TextWrapped(
        "Writes party composition (players/heroes/henchmen + professions) and how the run ended "
        "(wipe/resign/unknown) for each explorable-area run to PartyLog_YYYY-MM-DD.json in your "
        "GWToolbox runs folder, keyed by UTC start time so it can be joined against GWToolboxdll's "
        "own ObjectiveTimerRuns_*.json files.");
    if (last_written_utc_start) {
        std::string time_str;
        PluginUtils::TimeToString(last_written_utc_start, time_str);
        ImGui::Text("Last run logged: %s", time_str.c_str());
    }
}
