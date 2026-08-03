#include "UWSCTracker.h"

#include <Path.h> // Core: PathGetDocumentsPath / PathGetComputerName

#include <GWCA/Constants/Constants.h>
#include <GWCA/Constants/Maps.h>
#include <GWCA/Context/CharContext.h>
#include <GWCA/GameEntities/Agent.h>
#include <GWCA/GameEntities/Map.h> // full AreaInfo/RegionType definitions; MapMgr.h only forward-declares them
#include <GWCA/GameEntities/Party.h>
#include <GWCA/GameEntities/Player.h>
#include <GWCA/Managers/AgentMgr.h>
#include <GWCA/Managers/MapMgr.h>
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
#include <optional>
#include <sstream>

// Mirrors the shape written to disk; kept separate from the live UWSCTracker::PartyMember only in
// name, not in fields. Needs external linkage (i.e. can't live in an anonymous namespace) — glaze's
// reflection generates a stable name per type and errors (C7631) on internal-linkage types.
struct LogEntry {
    uint32_t utc_start = 0;
    uint32_t map_id = 0;
    std::string character_name;
    // "wipe", "resign", "completed", or "unknown". Set at run end from party/wipe signals alone
    // ("wipe" or "resign" or "unknown"); ProcessSync later upgrades "resign"/"unknown" to "completed"
    // once the matched GWToolboxdll objective data confirms the run actually finished.
    std::string end_reason;
    std::vector<UWSCTracker::PartyMember> party_members;
};

// Mirrors GWToolboxdll's ObjectiveTimerWindow::Objective::Serialized / ObjectiveSet::Serialized shape
// (Windows/ObjectiveTimerWindow.h) closely enough to read its runs/ObjectiveTimerRuns_*.json - can't
// include that header directly (internal to GWToolboxdll, not part of the exported plugin surface, and
// pulls in unrelated heavy deps like uWebSockets). Also needs external linkage, same reason as LogEntry.
struct RemoteObjective {
    std::string name;
    uint32_t status = 0; // 0=NotStarted, 1=Started, 2=Completed, 3=Failed
    uint32_t start = 0;
    uint32_t done = 0;
    std::optional<uint32_t> indent;
    std::optional<uint32_t> duration;
};
struct RemoteObjectiveSet {
    std::string name;
    uint32_t instance_start = 0;
    uint32_t utc_start = 0;
    std::vector<RemoteObjective> objectives;
    std::optional<uint32_t> duration;
};

// Body for the backend publish request.
struct PublishPayload {
    LogEntry party;
    std::optional<RemoteObjectiveSet> objective;
};

namespace {
    constexpr const char* kUploadRunsPath = "upload-runs"; // player configures the base URL; path is fixed
    constexpr uint64_t kSyncScanIntervalMs = 5 * 60 * 1000;      // rescan local files for new entries
    constexpr uint64_t kObjectiveGiveUpTimeoutMs = 10 * 60 * 1000; // publish without a matched objective past this
    constexpr uint64_t kRetryBackoffMs = 60 * 1000;               // wait this long before retrying a failed publish

    // The exact set of GW::Constants::MapID values ObjectiveTimerWindow::AddObjectiveSet()'s switch
    // statement matches (GWToolboxdll/Windows/ObjectiveTimerWindow.cpp) - i.e. every area it will
    // actually create an ObjectiveSet for. Everything else (random missions, vanquishes, etc.) is
    // skipped entirely: no capture, no resign/wipe hooks, no local write, since it could never find a
    // matching objective entry anyway. For multi-level dungeons only the entry level's id is listed,
    // matching the switch itself - later levels are tracked by the same already-active run, not by
    // this check firing again. DoA is deliberately excluded (see OnInstanceLoadInfo).
    //
    // NB: this is a manually maintained copy. If GWToolboxdll adds or removes a tracked area, this
    // list needs updating to match, or a newly-tracked area would get silently skipped here.
    const std::unordered_set<uint32_t> kTrackedMapIds = {
        // elite areas
        static_cast<uint32_t>(GW::Constants::MapID::Urgozs_Warren),
        static_cast<uint32_t>(GW::Constants::MapID::The_Deep),
        static_cast<uint32_t>(GW::Constants::MapID::The_Fissure_of_Woe),
        static_cast<uint32_t>(GW::Constants::MapID::The_Underworld),
        // dungeons - 1 level
        static_cast<uint32_t>(GW::Constants::MapID::Ooze_Pit),
        static_cast<uint32_t>(GW::Constants::MapID::Fronis_Irontoes_Lair_mission),
        static_cast<uint32_t>(GW::Constants::MapID::Secret_Lair_of_the_Snowmen),
        // dungeons - 2 levels (entry map id only)
        static_cast<uint32_t>(GW::Constants::MapID::Sepulchre_of_Dragrimmar_Level_1),
        static_cast<uint32_t>(GW::Constants::MapID::Bogroot_Growths_Level_1),
        static_cast<uint32_t>(GW::Constants::MapID::Arachnis_Haunt_Level_1),
        // dungeons - 3 levels (entry map id only)
        static_cast<uint32_t>(GW::Constants::MapID::Catacombs_of_Kathandrax_Level_1),
        static_cast<uint32_t>(GW::Constants::MapID::Rragars_Menagerie_Level_1),
        static_cast<uint32_t>(GW::Constants::MapID::Cathedral_of_Flames_Level_1),
        static_cast<uint32_t>(GW::Constants::MapID::Darkrime_Delves_Level_1),
        static_cast<uint32_t>(GW::Constants::MapID::Ravens_Point_Level_1),
        static_cast<uint32_t>(GW::Constants::MapID::Vloxen_Excavations_Level_1),
        static_cast<uint32_t>(GW::Constants::MapID::Bloodstone_Caves_Level_1),
        static_cast<uint32_t>(GW::Constants::MapID::Shards_of_Orr_Level_1),
        static_cast<uint32_t>(GW::Constants::MapID::Oolas_Lab_Level_1),
        static_cast<uint32_t>(GW::Constants::MapID::Heart_of_the_Shiverpeaks_Level_1),
        static_cast<uint32_t>(GW::Constants::MapID::Forsaken_Tunnels_Level1),
        static_cast<uint32_t>(GW::Constants::MapID::Forsaken_Tunnels_Presearing_Level1),
        // dungeons - 5 levels (entry map id only)
        static_cast<uint32_t>(GW::Constants::MapID::Frostmaws_Burrows_Level_1),
        // dungeons - irregular
        static_cast<uint32_t>(GW::Constants::MapID::Slavers_Exile_Level_5),
        // Tomb of the Primeval Kings (ToPK); ObjectiveTimerWindow additionally requires
        // GW::Map::GetCurrentMapInfo()->type == GW::RegionType::ExplorableZone to disambiguate
        // from the PvP-arena variant of this same map id - replicated in OnInstanceLoadInfo.
        static_cast<uint32_t>(GW::Constants::MapID::The_Underworld_PvP),
    };

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

    // GWToolboxdll's own file for the same UTC day (ObjectiveTimerWindow::SaveRuns, same gmtime bucketing).
    std::filesystem::path GetObjectiveLogFilePath(const uint32_t utc_start)
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
        wchar_t filename[48];
        swprintf(filename, _countof(filename), L"ObjectiveTimerRuns_%04d-%02d-%02d.json",
                 timeinfo->tm_year + 1900, timeinfo->tm_mon + 1, timeinfo->tm_mday);
        return folder / filename;
    }

    template <typename T>
    bool ReadJsonArray(const std::filesystem::path& path, std::vector<T>& out)
    {
        std::ifstream in{path};
        if (!in.is_open()) {
            return false;
        }
        std::stringstream ss;
        ss << in.rdbuf();
        constexpr glz::opts opts{.error_on_unknown_keys = false};
        return !glz::read<opts>(out, ss.str());
    }

    // Looks for a GWToolboxdll objective run matching utc_start, in today's and yesterday's files
    // (a run can start just before UTC midnight and be written just after).
    //
    // Not an exact match: GWToolboxdll stamps its utc_start once InstanceLoadInfo, InstanceLoadFile,
    // AND InstanceTimer have all arrived, while we stamp ours as soon as InstanceLoadInfo alone
    // arrives - if those don't land in the same frame, time()'s 1-second resolution can put the two
    // timestamps on opposite sides of a second boundary. Match the closest candidate within a small
    // tolerance instead of requiring equality.
    bool TryReadMatchingObjectiveEntry(const uint32_t utc_start, RemoteObjectiveSet& out)
    {
        constexpr uint32_t kMatchToleranceSec = 2;

        std::vector<RemoteObjectiveSet> sets;
        for (const uint32_t candidate_ts : {utc_start, utc_start - 86400u}) {
            std::vector<RemoteObjectiveSet> day_sets;
            if (ReadJsonArray(GetObjectiveLogFilePath(candidate_ts), day_sets)) {
                for (auto& s : day_sets) {
                    sets.push_back(std::move(s));
                }
            }
        }

        int best_index = -1;
        uint32_t best_diff = kMatchToleranceSec + 1;
        for (size_t i = 0; i < sets.size(); i++) {
            const uint32_t diff = sets[i].utc_start > utc_start
                ? sets[i].utc_start - utc_start
                : utc_start - sets[i].utc_start;
            if (diff <= kMatchToleranceSec && diff < best_diff) {
                best_diff = diff;
                best_index = static_cast<int>(i);
            }
        }
        if (best_index < 0) {
            return false;
        }
        out = std::move(sets[best_index]);
        return true;
    }

    // Resigning to leave after finishing a run is the normal exit mechanism, not a failure - it's
    // indistinguishable from an actual give-up resign at classification time (OnGameSrvTransfer, which
    // fires before GWToolboxdll has even written the objective data that would tell us which one it
    // was). GWToolboxdll only marks an objective Failed from StopObjectives() (a wipe); under normal
    // play every objective ends up Completed, and each area's Add*ObjectiveSet() appends its final
    // objective (e.g. Dhuum for UW) last - so objectives.back().status == Completed is a reliable
    // "this run actually finished" signal once we have it, checked here (ProcessSync, once the
    // objective entry is matched) rather than at classification time.
    constexpr uint32_t kObjectiveStatusCompleted = 2;
    bool IsRunCompleted(const RemoteObjectiveSet& objective_set)
    {
        return !objective_set.objectives.empty() && objective_set.objectives.back().status == kObjectiveStatusCompleted;
    }

    // Encoded prefix for the "<player> has resigned." system chat message. Not human-readable text -
    // it's GW's fixed per-template control-code sequence, so it matches regardless of the client's
    // display language (only the decoded text varies by language, not the encoded template id). Same
    // bytes GWToolboxdll's own ResignLogModule matches on.
    constexpr wchar_t kResignedPrefix[] = L"\x7BFF\xC9C4\xAEAA\x1B9B\x107";
}

DLLAPI ToolboxPlugin* ToolboxPluginInstance()
{
    static UWSCTracker instance;
    return &instance;
}

void UWSCTracker::Initialize(ImGuiContext* ctx, const ImGuiAllocFns allocator_fns, const HMODULE toolbox_dll)
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

void UWSCTracker::Terminate()
{
    GW::UI::RemoveUIMessageCallback(&WriteToChatLog_HookEntry, GW::UI::UIMessage::kWriteToChatLog);
    GW::StoC::RemoveCallback<GW::Packet::StoC::PartyDefeated>(&PartyDefeated_HookEntry);
    GW::StoC::RemoveCallback<GW::Packet::StoC::GameSrvTransfer>(&GameSrvTransfer_HookEntry);
    GW::StoC::RemoveCallback<GW::Packet::StoC::InstanceLoadInfo>(&InstanceLoadInfo_HookEntry);
    ToolboxPlugin::Terminate();
}

void UWSCTracker::OnInstanceLoadInfo(const uint32_t map_id, const bool is_explorable)
{
    if (!is_explorable || !kTrackedMapIds.contains(map_id)) {
        return; // not an area ObjectiveTimerWindow tracks; skip capture entirely for this instance
    }
    if (map_id == static_cast<uint32_t>(GW::Constants::MapID::The_Underworld_PvP)) {
        // Same map id is shared with a PvP arena variant; ObjectiveTimerWindow only tracks the
        // ToPK (explorable) one.
        const GW::AreaInfo* info = GW::Map::GetCurrentMapInfo();
        if (!info || info->type != GW::RegionType::ExplorableZone) {
            return;
        }
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

void UWSCTracker::OnPartyDefeated()
{
    if (!run_active) {
        return;
    }
    wipe_detected = true;
}

void UWSCTracker::OnWriteToChatLog(const wchar_t* message)
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

void UWSCTracker::OnGameSrvTransfer()
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
    WriteLogEntry(pending_utc_start, pending_map_id, pending_character_name, end_reason, party_members);
    last_queue_scan_tick = 0; // force ProcessSync to pick this up on the next tick, not the 5-minute cadence
}

void UWSCTracker::Update(float)
{
    CaptureParty();
    ProcessSync();
}

void UWSCTracker::CaptureParty()
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
        party_members.push_back(UWSCTracker::PartyMember{
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

// Takes explicit parameters (rather than reading pending_* member state) so ProcessSync can also call
// this to correct an already-written entry's end_reason once objective data reveals the true outcome
// (see the completed-run override in ProcessSync), not just OnGameSrvTransfer for the live capture.
void UWSCTracker::WriteLogEntry(const uint32_t utc_start, const uint32_t map_id, const std::string& character_name,
                                 const std::string& end_reason, const std::vector<PartyMember>& members)
{
    if (members.empty()) {
        return;
    }
    const auto path = GetLogFilePath(utc_start);
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

        // Replace any earlier entry for the same run (e.g. a district hop re-firing InstanceLoadInfo,
        // or ProcessSync correcting a previously-written end_reason).
        std::erase_if(entries, [&](const LogEntry& e) {
            return e.utc_start == utc_start && e.character_name == character_name;
        });
        entries.push_back(LogEntry{
            .utc_start = utc_start,
            .map_id = map_id,
            .character_name = character_name,
            .end_reason = end_reason,
            .party_members = members,
        });

        std::ofstream out{path};
        if (out.is_open()) {
            out << glz::write_json(entries).value_or(std::string{});
            last_written_utc_start = utc_start;
        }
    } catch (const std::exception&) {
        // Best-effort logging; nothing to do if the runs folder is unwritable.
    }
}

void UWSCTracker::RefreshSyncQueue()
{
    std::unordered_set<uint32_t> already_queued;
    for (const auto& q : sync_queue) {
        already_queued.insert(q.utc_start);
    }

    const uint32_t now_utc = static_cast<uint32_t>(time(nullptr));
    for (const uint32_t candidate_ts : {now_utc, now_utc - 86400u}) {
        std::vector<LogEntry> entries;
        if (!ReadJsonArray(GetLogFilePath(candidate_ts), entries)) {
            continue;
        }
        for (auto& e : entries) {
            if (e.utc_start > last_persisted_utc_start && !already_queued.contains(e.utc_start)) {
                sync_queue.push_back(SyncQueueEntry{
                    .utc_start = e.utc_start,
                    .map_id = e.map_id,
                    .character_name = std::move(e.character_name),
                    .end_reason = std::move(e.end_reason),
                    .party_members = std::move(e.party_members),
                    .first_seen_tick = GetTickCount64(),
                });
                already_queued.insert(e.utc_start);
            }
        }
    }

    std::ranges::sort(sync_queue, {}, &SyncQueueEntry::utc_start);
}

void UWSCTracker::ProcessSync()
{
    if (base_url.empty() || machine_key.empty()) {
        return; // publishing not configured; local PartyLog_*.json write is still the durable record
    }

    const uint64_t now = GetTickCount64();

    if (publish_request) {
        if (!publish_request->IsCompleted()) {
            return; // in flight
        }
        if (publish_request->IsSuccessful()) {
            last_persisted_utc_start = publishing_utc_start;
            if (!settings_folder.empty()) {
                SaveSettings(settings_folder.c_str()); // persist the watermark now, not on the host's cadence
            }
            if (!sync_queue.empty() && sync_queue.front().utc_start == publishing_utc_start) {
                sync_queue.pop_front();
            }
        }
        else {
            last_publish_attempt_tick = now; // back off before retrying a failed publish
        }
        publish_request.reset();
    }

    if (now - last_publish_attempt_tick < kRetryBackoffMs) {
        return;
    }

    if (now - last_queue_scan_tick >= kSyncScanIntervalMs) {
        last_queue_scan_tick = now;
        RefreshSyncQueue();
    }
    if (sync_queue.empty()) {
        return;
    }

    auto& next = sync_queue.front();
    RemoteObjectiveSet objective_set;
    const bool have_objective = TryReadMatchingObjectiveEntry(next.utc_start, objective_set);
    const bool gave_up_waiting = (now - next.first_seen_tick) >= kObjectiveGiveUpTimeoutMs;
    if (!have_objective && !gave_up_waiting) {
        return; // wait for GWToolboxdll's own file to catch up; retried next tick
    }

    // Now that we have the objective data, correct a resign/unknown classification if the run actually
    // finished (e.g. resigning right after killing Dhuum shouldn't read as giving up). Leave "wipe" as
    // reported - a genuine death event stays notable even in the rare case it's right after a kill.
    // Also corrects the local PartyLog_*.json entry, not just the published payload.
    if (have_objective && next.end_reason != "wipe" && next.end_reason != "completed" && IsRunCompleted(objective_set)) {
        next.end_reason = "completed";
        WriteLogEntry(next.utc_start, next.map_id, next.character_name, next.end_reason, next.party_members);
    }

    PublishPayload payload{
        .party = LogEntry{
            .utc_start = next.utc_start,
            .map_id = next.map_id,
            .character_name = next.character_name,
            .end_reason = next.end_reason,
            .party_members = next.party_members,
        },
    };
    if (have_objective) {
        payload.objective = std::move(objective_set);
    }

    std::string url;
    ComposeUrl(url, base_url.c_str(), kUploadRunsPath);

    publish_request = std::make_unique<AsyncRestClient>();
    publish_request->SetUrl(url.c_str());
    publish_request->SetMethod(HttpMethod::Post);
    publish_request->SetHeader("Content-Type", "application/json");
    publish_request->SetHeader("X-Machine-Key", machine_key.c_str());
    publish_request->SetPostContent(glz::write_json(payload).value_or(std::string{}), ContentFlag::Copy);
    publish_request->SetTimeoutSec(10);
    publish_request->SetConnectTimeoutSec(5);
    publish_request->SetVerifyPeer(true);
    publish_request->SetVerifyHost(true);
    publishing_utc_start = next.utc_start;
    publish_request->ExecuteAsync();
}

void UWSCTracker::LoadSettings(const wchar_t* folder)
{
    ToolboxPlugin::LoadSettings(folder);
    settings_folder = folder;
    LoadSetting("base_url", base_url);
    LoadSetting("machine_key", machine_key);
    LoadSetting("last_persisted_utc_start", last_persisted_utc_start);
    PluginUtils::StrCopy(base_url_buf, base_url.c_str(), sizeof(base_url_buf));
    PluginUtils::StrCopy(machine_key_buf, machine_key.c_str(), sizeof(machine_key_buf));
}

void UWSCTracker::SaveSettings(const wchar_t* folder)
{
    settings_folder = folder;
    SaveSetting("base_url", base_url);
    SaveSetting("machine_key", machine_key);
    SaveSetting("last_persisted_utc_start", last_persisted_utc_start);
    ToolboxPlugin::SaveSettings(folder);
}

void UWSCTracker::DrawSettings()
{
    ImGui::TextWrapped(
        "Writes party composition (players/heroes/henchmen + professions) and how the run ended "
        "(wipe/resign/completed/unknown) for each explorable-area run to PartyLog_YYYY-MM-DD.json in "
        "your GWToolbox runs folder, keyed by UTC start time so it can be joined against "
        "GWToolboxdll's own ObjectiveTimerRuns_*.json files.");
    if (last_written_utc_start) {
        std::string time_str;
        PluginUtils::TimeToString(last_written_utc_start, time_str);
        ImGui::Text("Last run logged: %s", time_str.c_str());
    }

    ImGui::Separator();
    ImGui::TextWrapped(
        "Backend sync: periodically publishes each run (party + matched objective data, once "
        "GWToolboxdll has written it) to <Base URL>/upload-runs, authenticated via the "
        "X-Machine-Key header. Leave the URL blank to disable and keep local logging only.");
    if (ImGui::InputText("Base URL", base_url_buf, sizeof(base_url_buf))) {
        base_url = base_url_buf;
    }
    if (ImGui::InputText("Machine Key", machine_key_buf, sizeof(machine_key_buf), ImGuiInputTextFlags_Password)) {
        machine_key = machine_key_buf;
    }
    ImGui::Text("Sync queue: %zu pending", sync_queue.size());
    if (last_persisted_utc_start) {
        std::string time_str;
        PluginUtils::TimeToString(last_persisted_utc_start, time_str);
        ImGui::Text("Last synced run: %s", time_str.c_str());
    }
}
