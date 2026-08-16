#include "SCTracker.h"
#include "PluginVersion.generated.h" // kPluginVersion - see cmake/gwtoolboxdll_plugins.cmake

#include <Path.h> // Core: PathGetDocumentsPath / PathGetComputerName

#include <GWCA/Constants/AgentIDs.h>
#include <GWCA/Constants/Constants.h>
#include <GWCA/Constants/ItemIDs.h>
#include <GWCA/Constants/Maps.h>
#include <GWCA/Constants/Skills.h>
#include <GWCA/Context/CharContext.h>
#include <GWCA/GameEntities/Agent.h>
#include <GWCA/GameEntities/Hero.h> // full HeroInfo definition; PartyMgr.h only forward-declares it
#include <GWCA/GameEntities/Map.h> // full AreaInfo/RegionType definitions; MapMgr.h only forward-declares them
#include <GWCA/GameEntities/Party.h>
#include <GWCA/GameEntities/Player.h>
#include <GWCA/GameEntities/Skill.h>
#include <GWCA/Managers/AgentMgr.h>
#include <GWCA/Managers/ChatMgr.h>
#include <GWCA/Managers/MapMgr.h>
#include <GWCA/Managers/PartyMgr.h>
#include <GWCA/Managers/PlayerMgr.h>
#include <GWCA/Managers/SkillbarMgr.h>
#include <GWCA/Managers/StoCMgr.h>
// GWCA/Constants/UIMessages.h has no include guard - don't include it directly. UIMgr.h (which does
// have one) already pulls it in internally; including both causes its content to be pasted twice in
// this TU, which corrupts parsing for the rest of that file and shows up as bogus "undeclared
// identifier"/"undefined type" errors for symbols defined later in it.
#include <GWCA/Managers/UIMgr.h>
#include <GWCA/Packets/StoC.h>

#include <glaze/glaze.hpp>

#include <filesystem>
#include <format>
#include <fstream>
#include <optional>
#include <sstream>

// Mirrors the shape written to disk; kept separate from the live SCTracker::PartyMember only in
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
    std::vector<SCTracker::PartyMember> party_members;
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

// Body for the backend publish request. objective is always populated by the time this is
// constructed - ProcessSync drops (rather than publishes) any run that never gets a matching
// GWToolboxdll objective entry, since party-only data can never be leaderboard-eligible anyway.
struct PublishPayload {
    LogEntry party;
    RemoteObjectiveSet objective;
};

// Only the fields ProcessSync needs from a successful /upload-run response body - run_id (absent
// when the upload was silently dropped, e.g. an outdated plugin build) drives whether/which run
// the failure-report popup opens for. Needs external linkage, same reason as LogEntry.
struct UploadRunResponseDto {
    std::optional<int64_t> run_id;
    bool created = false;
};

// Body for POST /report-run-failure. Needs external linkage, same reason as LogEntry.
struct ReportFailurePayload {
    int64_t run_id = 0;
    std::vector<std::string> roles;
};

// Response body for GET /can-report-run-failure. Needs external linkage, same reason as LogEntry.
struct CanReportFailureResponseDto {
    bool can_report_failures = false;
};

// Response body for GET /plugin-version. Needs external linkage, same reason as LogEntry.
struct PluginVersionResponseDto {
    int version = 0;
    std::string compiled_at;
};

namespace {
    constexpr const char* kBaseUrl = "https://gwsctracker.com";
    constexpr const char* kUploadRunsPath = "upload-run";
    constexpr const char* kReportFailurePath = "report-run-failure";
    constexpr const char* kCanReportFailurePath = "can-report-run-failure";
    constexpr const char* kPluginVersionPath = "plugin-version";
    constexpr int kHttpStatusUpgradeRequired = 426;

    // Static role vocabulary for the failure-report popup, mirroring the backend's RoleDerivation
    // output exactly (T1-T3 from the plugin's own role_hint, the rest from server-side profession-combo
    // derivation the plugin has no visibility into) - see SCTracker::failure_role_checked's comment.
    // "Nobody" (no player at fault - e.g. a disconnect, lag spike, or bad luck) records a run_failure_reasons
    // row with no run_participant attached - see FailureReportService.submit on the backend.
    constexpr std::array<const char*, 12> kFailureReasonRoles = {
        "T1", "T2", "T3", "T4", "LT", "Spiker", "Derv", "SoS", "Necro", "RangerNecro", "Emo", "Nobody",
    };
    // "Nobody" must stay last in kFailureReasonRoles - DrawFailurePopup uses this index to enforce
    // mutual exclusivity between it and every other reason (checking one clears the other(s)).
    constexpr size_t kNobodyReasonIndex = kFailureReasonRoles.size() - 1;
    constexpr uint64_t kSyncScanIntervalMs = 5 * 60 * 1000;      // rescan local files for new entries
    constexpr uint64_t kObjectiveGiveUpTimeoutMs = 10 * 60 * 1000; // publish without a matched objective past this
    constexpr uint64_t kRetryBackoffMs = 60 * 1000;               // wait this long before retrying a failed publish
    constexpr uint32_t kDeathTrackingGraceSec = 60; // ignore deaths in the first minute of the instance

    // Marks Dhuum's agent turning hostile (GAME_SMSG_AGENT_UPDATE_ALLEGIANCE). Same signal
    // ObjectiveTimerWindow::AddUWObjectiveSet() uses to start its "Dhuum" objective
    // (GWToolboxdll/Windows/ObjectiveTimerWindow.cpp) - mirrored here rather than read from that
    // module, since plugins can't include GWToolboxdll's internal headers.
    constexpr uint32_t kDhuumHostileAllegianceBits = 0x6D6F6E31;

    // Native mission-objective id for UW's "Dhuum" objective (GAME_SMSG_MISSION_OBJECTIVE_COMPLETE).
    // Same id ObjectiveTimerWindow::AddUWObjectiveSet() uses to end its own "Dhuum" objective
    // (GWToolboxdll/Windows/ObjectiveTimerWindow.cpp) - mirrored here for the same reason as
    // kDhuumHostileAllegianceBits above.
    constexpr uint32_t kDhuumObjectiveId = 157;

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

    // Own log file, separate from GWToolboxdll's log.txt (which plugins can't write to - it's not part
    // of the exported surface, and Debug builds hold it open without shared-write access). Lives next to
    // the runs/ folder rather than in it, since it isn't run data.
    void AppendLog(const std::string& line)
    {
        const auto runs_folder = GetRunsFolder();
        if (runs_folder.empty()) {
            return;
        }
        std::ofstream out{runs_folder.parent_path() / L"SCTracker.log", std::ios::app};
        if (!out.is_open()) {
            return;
        }
        const time_t now = time(nullptr);
        char ts[32] = "";
        if (const tm* timeinfo = gmtime(&now)) {
            strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", timeinfo);
        }
        out << '[' << ts << "] " << line << '\n';
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

    // party_members.size() alone isn't enough to identify a real 8-man guild run - a solo player
    // filling the other 7 slots with heroes/henchmen also occupies all 8 slots. Count only real
    // players (is_player == true) instead.
    uint32_t CountRealPlayers(const std::vector<SCTracker::PartyMember>& members)
    {
        return static_cast<uint32_t>(std::ranges::count_if(members, &SCTracker::PartyMember::is_player));
    }

    constexpr const char* kUnknownRole = "unknown";

    // The t1/t2/t3 role archetype is specifically Ranger-primary/Assassin-secondary (2/7) - a Ranger
    // primary with some other secondary (e.g. Ranger/Necromancer), or an Assassin primary, isn't part
    // of it, even if they incidentally use some of the same skills for unrelated reasons. Used to gate
    // role_skills/role_hint tracking in both OnSkillUsed and ProcessTrackedSkillUse - keep both in sync
    // with this.
    bool IsRoleEligible(const uint32_t primary, const uint32_t secondary)
    {
        return static_cast<GW::Constants::Profession>(primary) == GW::Constants::Profession::Ranger
            && static_cast<GW::Constants::Profession>(secondary) == GW::Constants::Profession::Assassin;
    }

    // Every skill relevant to t2/t3 (see kRoleCombos and OnSkillUsed), matched by decoded English name
    // rather than SkillID: the SkillID enum has no explicit per-entry numbering (Skills.h just lists
    // them in order), so its ordinals shift whenever GWCA's header gains or loses an entry anywhere
    // earlier in the list - a decoded name is stable regardless. Forced to English (see OnSkillUsed's
    // skill_name_cache population) rather than following the client's language setting. Populates
    // PartyMember::role_skills whenever the LOCAL PLAYER uses one of these, independent of whether a
    // role can actually be determined from it.
    const std::unordered_set<std::string> kTrackedSkillNameSet = {
        "Shadow of Haste",  "Shadow Walk",       "Winnowing",         "Finish Him!",
        "Recall",           "Radiation Field",   "Viper's Defense",   "Edge of Extinction",
        "Quickening Zephyr",
    };

    struct RoleCombo {
        const char* role;
        std::vector<std::string> required_skills; // ALL must appear in role_skills to satisfy this combo
    };

    // role_hint is set to the role of the first of these combos whose required_skills are all present
    // in the local player's role_skills (see OnSkillUsed) - i.e. this is always about the uploader's
    // own, reliably-observed skill usage, never a guess about someone else. t1 combos are listed first
    // so they're preferred if a member's skills happen to satisfy more than one role's combo at once.
    // Radiation Field alone is sufficient for t2; Viper's Defense is tracked (kTrackedSkillNameSet) but
    // doesn't itself factor into any combo below.
    const std::vector<RoleCombo> kRoleCombos = {
        {"t1", {"Shadow of Haste", "Shadow Walk"}},
        {"t1", {"Winnowing", "Finish Him!"}},
        {"t1", {"Shadow of Haste", "Recall"}},
        {"t2", {"Radiation Field"}},
        {"t3", {"Shadow of Haste", "Edge of Extinction"}},
        {"t3", {"Shadow of Haste", "Quickening Zephyr"}},
    };

    // model_ids counted into PartyMember::item_drops (see OnItemGeneral/OnItemUpdateOwner). model_id,
    // not item_id, identifies an item type: item_id is per-drop-instance and gets recycled
    // (GAME_SMSG_ITEM_REUSE_ID) - model_id is what GWCA's own item APIs (GetItemByModelId etc.,
    // ItemMgr.h) key on instead, and what's sent to the backend (PartyMember::ItemDropCount::id) - the
    // backend is expected to already have its own id -> display name mapping.
    const std::unordered_set<uint32_t> kTrackedItems = {
        GW::Constants::ItemID::GlobofEctoplasm, // Glob of Ectoplasm
        GW::Constants::ItemID::VoltaicSpear,    // Voltaic Spear
        GW::Constants::ItemID::DSR,             // DSR
        GW::Constants::ItemID::EternalBlade,    // Eternal Blade
        GW::Constants::ItemID::MiniDhuum,       // Mini Dhuum
    };

    // Encoded prefix for the "<player> has resigned." system chat message. Not human-readable text -
    // it's GW's fixed per-template control-code sequence, so it matches regardless of the client's
    // display language (only the decoded text varies by language, not the encoded template id). Same
    // bytes GWToolboxdll's own ResignLogModule matches on.
    constexpr wchar_t kResignedPrefix[] = L"\x7BFF\xC9C4\xAEAA\x1B9B\x107";
}

DLLAPI ToolboxPlugin* ToolboxPluginInstance()
{
    static SCTracker instance;
    return &instance;
}

void SCTracker::Initialize(ImGuiContext* ctx, const ImGuiAllocFns allocator_fns, const HMODULE toolbox_dll)
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
    GW::StoC::RegisterPacketCallback<GW::Packet::StoC::AgentState>(
        &AgentState_HookEntry,
        [this](GW::HookStatus*, const GW::Packet::StoC::AgentState* packet) {
            OnUpdateAgentState(packet->agent_id, packet->state);
        });
    GW::StoC::RegisterPacketCallback<GW::Packet::StoC::AgentUpdateAllegiance>(
        &AgentUpdateAllegiance_HookEntry,
        [this](GW::HookStatus*, const GW::Packet::StoC::AgentUpdateAllegiance* packet) {
            OnAgentUpdateAllegiance(packet->agent_id, packet->allegiance_bits);
        });
    GW::StoC::RegisterPacketCallback<GW::Packet::StoC::ObjectiveDone>(
        &ObjectiveDone_HookEntry, [this](GW::HookStatus*, const GW::Packet::StoC::ObjectiveDone* packet) {
            OnObjectiveDone(packet->objective_id);
        });
    // Skill used on self / no target.
    GW::StoC::RegisterPacketCallback<GW::Packet::StoC::GenericValue>(
        &GenericValueSelf_HookEntry,
        [this](GW::HookStatus*, const GW::Packet::StoC::GenericValue* packet) {
            switch (packet->value_id) {
                case GW::Packet::StoC::GenericValueID::instant_skill_activated:
                case GW::Packet::StoC::GenericValueID::skill_activated:
                case GW::Packet::StoC::GenericValueID::skill_finished:
                case GW::Packet::StoC::GenericValueID::attack_skill_activated:
                case GW::Packet::StoC::GenericValueID::attack_skill_finished:
                    OnSkillUsed(packet->agent_id, static_cast<GW::Constants::SkillID>(packet->value));
                    break;
                default:
                    break;
            }
        });
    // Skill used on a target. Field names are misleading for these event ids: per GenericValueID's
    // own comments in StoC.h ("caster_id is victim and target_id is caster"), and confirmed by
    // PartyStatisticsWindow's SkillCallback (GWToolboxdll/Windows/PartyStatisticsWindow.cpp), the
    // actual caster is packet->target, not packet->caster.
    GW::StoC::RegisterPacketCallback<GW::Packet::StoC::GenericValueTarget>(
        &GenericValueTarget_HookEntry,
        [this](GW::HookStatus*, const GW::Packet::StoC::GenericValueTarget* packet) {
            switch (packet->Value_id) {
                case GW::Packet::StoC::GenericValueID::instant_skill_activated:
                case GW::Packet::StoC::GenericValueID::skill_activated:
                case GW::Packet::StoC::GenericValueID::skill_finished:
                case GW::Packet::StoC::GenericValueID::attack_skill_activated:
                case GW::Packet::StoC::GenericValueID::attack_skill_finished:
                    OnSkillUsed(packet->target, static_cast<GW::Constants::SkillID>(packet->value));
                    break;
                default:
                    break;
            }
        });
    GW::StoC::RegisterPacketCallback<GW::Packet::StoC::ItemGeneral>(
        &ItemGeneral_HookEntry, [this](GW::HookStatus*, const GW::Packet::StoC::ItemGeneral* packet) {
            OnItemGeneral(packet->item_id, packet->model_id);
        });
    GW::StoC::RegisterPacketCallback<GW::Packet::StoC::ItemUpdateOwner>(
        &ItemUpdateOwner_HookEntry, [this](GW::HookStatus*, const GW::Packet::StoC::ItemUpdateOwner* packet) {
            OnItemUpdateOwner(packet->item_id, packet->owner_agent_id);
        });
    GW::UI::RegisterUIMessageCallback(
        &WriteToChatLog_HookEntry, GW::UI::UIMessage::kWriteToChatLog,
        [this](GW::HookStatus*, GW::UI::UIMessage, void* wParam, void*) {
            OnWriteToChatLog(static_cast<GW::UI::UIPacket::kWriteToChatLog*>(wParam)->message);
        },
        0x8000);
}

void SCTracker::Terminate()
{
    GW::UI::RemoveUIMessageCallback(&WriteToChatLog_HookEntry, GW::UI::UIMessage::kWriteToChatLog);
    GW::StoC::RemoveCallback<GW::Packet::StoC::ItemUpdateOwner>(&ItemUpdateOwner_HookEntry);
    GW::StoC::RemoveCallback<GW::Packet::StoC::ItemGeneral>(&ItemGeneral_HookEntry);
    GW::StoC::RemoveCallback<GW::Packet::StoC::GenericValueTarget>(&GenericValueTarget_HookEntry);
    GW::StoC::RemoveCallback<GW::Packet::StoC::GenericValue>(&GenericValueSelf_HookEntry);
    GW::StoC::RemoveCallback<GW::Packet::StoC::ObjectiveDone>(&ObjectiveDone_HookEntry);
    GW::StoC::RemoveCallback<GW::Packet::StoC::AgentUpdateAllegiance>(&AgentUpdateAllegiance_HookEntry);
    GW::StoC::RemoveCallback<GW::Packet::StoC::AgentState>(&AgentState_HookEntry);
    GW::StoC::RemoveCallback<GW::Packet::StoC::PartyDefeated>(&PartyDefeated_HookEntry);
    GW::StoC::RemoveCallback<GW::Packet::StoC::GameSrvTransfer>(&GameSrvTransfer_HookEntry);
    GW::StoC::RemoveCallback<GW::Packet::StoC::InstanceLoadInfo>(&InstanceLoadInfo_HookEntry);
    ToolboxPlugin::Terminate();
}

void SCTracker::OnInstanceLoadInfo(const uint32_t map_id, const bool is_explorable)
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
    dhuum_started = false;
    dhuum_completed = false;
    tracked_item_id_to_model_id.clear();
    // Not skill_name_cache itself (deliberately persists - see its declaration) - just events still
    // queued from the previous run. Decoding is near-instant in practice, but without this a very
    // late-finishing decode could otherwise attribute a previous run's skill use to this new one.
    pending_role_skill_events.clear();
}

void SCTracker::OnPartyDefeated()
{
    if (!run_active) {
        return;
    }
    wipe_detected = true;
}

void SCTracker::OnWriteToChatLog(const wchar_t* message)
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

// GAME_SMSG_AGENT_UPDATE_EFFECTS; state bit 0x0010 is the agent's dead flag. Only counts the
// alive->dead edge (not every packet while already dead) and re-arms on the dead->alive edge (i.e. a
// resurrection), so a member who dies twice in the same run is counted twice. Deaths in the first
// minute of the instance (loading in, initial positioning, an early accidental pull) aren't counted -
// party_member_currently_dead is deliberately left unsynced during that window, same as dhuum_started:
// the first real edge evaluated after the grace period compares against whatever it defaulted to,
// which self-corrects rather than needing to be back-filled.
void SCTracker::OnUpdateAgentState(const uint32_t agent_id, const uint32_t state)
{
    if (!run_active || dhuum_started) {
        return;
    }
    if (static_cast<uint32_t>(time(nullptr)) - pending_utc_start < kDeathTrackingGraceSec) {
        return;
    }
    const auto it = agent_id_to_party_index.find(agent_id);
    if (it == agent_id_to_party_index.end()) {
        return;
    }
    const size_t idx = it->second;
    const bool now_dead = (state & 0x0010) != 0;
    if (now_dead == party_member_currently_dead[idx]) {
        return;
    }
    party_member_currently_dead[idx] = now_dead;
    if (now_dead) {
        party_members[idx].deaths++;
    }
}

// Fires on Dhuum's agent turning hostile (living->player_number doubles as the model id for NPC
// agents - see AgentIDs.h's ModelID namespace comment). Latches dhuum_started for the rest of the
// run; OnUpdateAgentState stops counting deaths once it's set, since deaths during/after the Dhuum
// fight (e.g. the tank) are expected, not run-ending mistakes.
void SCTracker::OnAgentUpdateAllegiance(const uint32_t agent_id, const uint32_t allegiance_bits)
{
    if (!run_active || dhuum_started || allegiance_bits != kDhuumHostileAllegianceBits) {
        return;
    }
    const GW::Agent* agent = GW::Agents::GetAgentByID(agent_id);
    const GW::AgentLiving* living = agent ? agent->GetAsAgentLiving() : nullptr;
    if (living && living->player_number == static_cast<uint32_t>(GW::Constants::ModelID::UW::Dhuum)) {
        dhuum_started = true;
    }
}

// GAME_SMSG_MISSION_OBJECTIVE_COMPLETE - fires for any completed native mission objective, not just
// Dhuum's; only kDhuumObjectiveId is relevant here. Latches dhuum_completed for the rest of the run;
// OnItemGeneral stops counting Glob of Ectoplasm drops once it's set.
void SCTracker::OnObjectiveDone(const uint32_t objective_id)
{
    if (!run_active || objective_id != kDhuumObjectiveId) {
        return;
    }
    dhuum_completed = true;
}

// Role tracking is local-player-only now (see PartyMember::role_skills' comment) - bails immediately
// for any other agent, before touching skill_name_cache at all. For the local player, ensures a decode
// is in flight for skill_id (starting one via skill_name_cache if this is the first time it's been
// seen at all, this run or any previous one), then either processes immediately (already decoded, e.g.
// a repeat cast) or queues the event for FlushPendingRoleSkills to pick up once decoding finishes.
void SCTracker::OnSkillUsed(const uint32_t agent_id, const GW::Constants::SkillID skill_id)
{
    if (!run_active || skill_id == GW::Constants::SkillID::No_Skill) {
        return;
    }
    if (agent_id != GW::Agents::GetControlledCharacterId()) {
        return;
    }
    const auto member_it = agent_id_to_party_index.find(agent_id);
    if (member_it == agent_id_to_party_index.end()) {
        return;
    }
    const PartyMember& candidate = party_members[member_it->second];
    if (!IsRoleEligible(candidate.primary, candidate.secondary)) {
        return;
    }

    const auto id = static_cast<uint32_t>(skill_id);
    auto cache_it = skill_name_cache.find(id);
    if (cache_it == skill_name_cache.end()) {
        const GW::Skill* skill_data = GW::SkillbarMgr::GetSkillConstantData(skill_id);
        auto enc = std::make_unique<PluginUtils::EncString>(skill_data ? skill_data->name : 0u);
        enc->language(GW::Constants::Language::English);
        enc->wstring(); // trigger decode
        cache_it = skill_name_cache.emplace(id, std::move(enc)).first;
    }
    if (cache_it->second->IsDecoding()) {
        pending_role_skill_events.push_back({.skill_id = id});
        return;
    }
    ProcessTrackedSkillUse(cache_it->second->string());
}

// Drains pending_role_skill_events, calling ProcessTrackedSkillUse for any whose skill_name_cache
// entry has finished decoding. Called from Update.
void SCTracker::FlushPendingRoleSkills()
{
    std::erase_if(pending_role_skill_events, [this](const PendingRoleSkillEvent& event) {
        const auto it = skill_name_cache.find(event.skill_id);
        if (it == skill_name_cache.end() || it->second->IsDecoding()) {
            return false; // shouldn't happen (the cache entry always exists by the time it's queued),
                           // but leave it queued rather than drop it if it somehow does
        }
        ProcessTrackedSkillUse(it->second->string());
        return true;
    });
}

// Re-resolves the local player's PartyMember entry (rather than being passed one, since OnSkillUsed
// already guaranteed agent_id == the controlled character before ever queuing this) and re-checks
// eligibility, since party composition/profession, in principle, could change between OnSkillUsed
// queuing this and it actually running. Records a kTrackedSkillNameSet hit into role_skills (deduped),
// then - only while role_hint is still "unknown" - checks kRoleCombos in order and locks in the role of
// the first fully-satisfied combo. Once role_hint is set it's never changed again for the rest of the
// run.
void SCTracker::ProcessTrackedSkillUse(const std::string& skill_name)
{
    if (!run_active || !kTrackedSkillNameSet.contains(skill_name)) {
        return;
    }
    const auto member_it = agent_id_to_party_index.find(GW::Agents::GetControlledCharacterId());
    if (member_it == agent_id_to_party_index.end()) {
        return;
    }
    PartyMember& member = party_members[member_it->second];
    if (!IsRoleEligible(member.primary, member.secondary)) {
        return;
    }
    if (std::ranges::contains(member.role_skills, skill_name)) {
        return; // already recorded - nothing new to (re-)evaluate
    }
    member.role_skills.push_back(skill_name);

    if (member.role_hint != kUnknownRole) {
        return; // already locked in
    }
    for (const auto& combo : kRoleCombos) {
        const bool satisfied = std::ranges::all_of(combo.required_skills, [&](const std::string& s) {
            return std::ranges::contains(member.role_skills, s);
        });
        if (satisfied) {
            member.role_hint = combo.role;
            break;
        }
    }
}

// GAME_SMSG_ITEM_GENERAL_INFO - fires for items as they're identified client-side (e.g. a drop
// landing). Caches item_id -> model_id only for kTrackedItems hits, so OnItemUpdateOwner has
// something to resolve the item_id it gets to. Untracked items are never cached, keeping this bounded
// to however many tracked-item drops are in flight at once. Glob of Ectoplasm specifically stops being
// cached (and therefore counted) once dhuum_completed is set - other tracked items are unaffected.
void SCTracker::OnItemGeneral(const uint32_t item_id, const uint32_t model_id)
{
    if (!run_active || !kTrackedItems.contains(model_id)) {
        return;
    }
    if (dhuum_completed && model_id == static_cast<uint32_t>(GW::Constants::ItemID::GlobofEctoplasm)) {
        return;
    }
    tracked_item_id_to_model_id[item_id] = model_id;
}

// GAME_SMSG_ITEM_UPDATE_OWNER - loot reservation, broadcast to the whole party (not just the
// recipient). Can re-fire for the same item_id if the reservation is reassigned (GWToolboxdll's
// ItemDrops module tracks this by updating an owner map in place, not counting) - only the first
// firing for a given tracked item_id is counted here, then the cache entry is erased so a later
// reassignment isn't double-counted. Reflects who it was reserved for, not confirmed pickup - another
// player's inventory contents beyond a reservation broadcast aren't visible to this client at all.
void SCTracker::OnItemUpdateOwner(const uint32_t item_id, const uint32_t owner_agent_id)
{
    if (!run_active) {
        return;
    }
    const auto model_it = tracked_item_id_to_model_id.find(item_id);
    if (model_it == tracked_item_id_to_model_id.end()) {
        return;
    }
    const uint32_t model_id = model_it->second;
    tracked_item_id_to_model_id.erase(model_it);

    const auto member_it = agent_id_to_party_index.find(owner_agent_id);
    if (member_it == agent_id_to_party_index.end()) {
        return;
    }
    auto& drops = party_members[member_it->second].item_drops;
    const auto drop_it = std::ranges::find(drops, model_id, &PartyMember::ItemDropCount::id);
    if (drop_it != drops.end()) {
        drop_it->count++;
    }
    else {
        drops.push_back({.id = model_id, .count = 1});
    }
}

void SCTracker::OnGameSrvTransfer()
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

void SCTracker::Update(float)
{
    CaptureParty();
    FlushPendingRoleSkills();
    ProcessSync();
    ProcessPermissionCheck();
    ProcessFailureSubmit();
    ProcessVersionCheck();
}

void SCTracker::CaptureParty()
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
        agent_id_to_party_index.clear();
        party_member_currently_dead.clear();
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

    // primary/secondary come from the party/player/hero roster data, not the agent - the roster is
    // populated as soon as party membership syncs, whereas another real player's in-world agent can
    // take a moment longer to spawn/load. Reading it off the agent here raced that load and silently
    // left late-loading members at primary=secondary=0 (i.e. indistinguishable from Profession::None).
    const auto add_member = [&](const uint32_t agent_id, const wchar_t* enc_name, const uint32_t primary,
                                 const uint32_t secondary, const bool is_player, const bool is_hero,
                                 const bool is_henchman) {
        party_members.push_back(SCTracker::PartyMember{
            .primary = primary,
            .secondary = secondary,
            .is_player = is_player,
            .is_hero = is_hero,
            .is_henchman = is_henchman
        });
        agent_id_to_party_index[agent_id] = party_members.size() - 1;
        party_member_currently_dead.push_back(false);
        // NB: Player may have left the game, meaning GW::Agents::GetAgentEncName(agent_id) would fail
        // because the agent is gone. Pass enc_name for real players instead.
        auto enc = std::make_unique<PluginUtils::EncString>();
        enc->reset(enc_name ? enc_name : GW::Agents::GetAgentEncName(agent_id));
        enc->wstring(); // trigger decode
        party_member_enc_names.push_back(std::move(enc));
    };

    for (const auto& player : info->players) {
        if (const GW::Player* gwplayer = GW::PlayerMgr::GetPlayerByID(player.login_number)) {
            add_member(gwplayer->agent_id, gwplayer->name_enc, gwplayer->primary, gwplayer->secondary,
                       true, false, false);
        }
    }
    for (const auto& hero : info->heroes) {
        uint32_t primary = 0;
        uint32_t secondary = 0;
        if (const GW::HeroInfo* hero_info = GW::PartyMgr::GetHeroInfo(hero.hero_id)) {
            primary = static_cast<uint32_t>(hero_info->primary);
            secondary = static_cast<uint32_t>(hero_info->secondary);
        }
        add_member(hero.agent_id, nullptr, primary, secondary, false, true, false);
    }
    for (const auto& hench : info->henchmen) {
        // HenchmanPartyMember exposes only a single profession field - no secondary.
        add_member(hench.agent_id, nullptr, static_cast<uint32_t>(hench.profession), 0, false, false, true);
    }

    if (party_members.empty()) {
        active_capture = false; // no party info found; nothing to log
    }
}

// Takes explicit parameters (rather than reading pending_* member state) so ProcessSync can also call
// this to correct an already-written entry's end_reason once objective data reveals the true outcome
// (see the completed-run override in ProcessSync), not just OnGameSrvTransfer for the live capture.
void SCTracker::WriteLogEntry(const uint32_t utc_start, const uint32_t map_id, const std::string& character_name,
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

void SCTracker::RefreshSyncQueue()
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

void SCTracker::ProcessSync()
{
    if (machine_key.empty()) {
        return; // publishing not configured; local PartyLog_*.json write is still the durable record
    }
    if (plugin_outdated) {
        return; // disabled until updated - see plugin_outdated's declaration
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
                // Open the failure-report popup for this run before popping it - need its
                // end_reason, which only lives on the queue entry, not the response body. Gated on
                // can_report_failures so none of this (including parsing the response body below)
                // runs at all when the server hasn't confirmed permission - see its declaration.
                const bool failed = can_report_failures &&
                    (sync_queue.front().end_reason == "wipe" || sync_queue.front().end_reason == "resign");
                UploadRunResponseDto response;
                constexpr glz::opts opts{.error_on_unknown_keys = false};
                if (failed && !glz::read<opts>(response, publish_request->GetContent()) && response.run_id) {
                    pending_failure_run_id = *response.run_id;
                    show_failure_popup = true;
                    failure_role_checked.fill(false);
                    failure_submit_error.clear();
                }
                sync_queue.pop_front();
            }
        }
        else {
            last_publish_attempt_tick = now; // back off before retrying a failed publish
            if (publish_request->GetStatusCode() == kHttpStatusUpgradeRequired) {
                plugin_outdated = true;
                if (!version_check_request) {
                    RequestLatestPluginVersion(); // refresh the exact version number for the DrawSettings message
                }
            }
            std::string body = publish_request->GetContent();
            if (body.size() > 200) {
                body.resize(200);
            }
            AppendLog(std::format("Publish failed for run {}: status={} http_code={} body={}",
                                   publishing_utc_start, publish_request->GetStatusStr(),
                                   publish_request->GetStatusCode(), body));
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

    // Drain every disqualified entry up front, regardless of queue position, so a stuck head-of-queue
    // entry (e.g. one that will never find a matching objective) doesn't block entries behind it from
    // ever being evaluated - the loop only stops at an entry that's either ready to publish or still
    // within its give-up window. Only full 8-man parties of real players are meaningful for the
    // leaderboard backend (a solo player filling the other 7 slots with heroes/henchmen still occupies
    // all 8 slots but isn't a guild run), and a run with no matching GWToolboxdll objective entry can
    // never be leaderboard-eligible anyway. Both cases mark the entry synced instead of retrying it
    // forever.
    bool advanced_watermark = false;
    RemoteObjectiveSet objective_set;
    bool have_objective = false;
    while (!sync_queue.empty()) {
        auto& front = sync_queue.front();
        if (CountRealPlayers(front.party_members) != 8) {
            last_persisted_utc_start = front.utc_start;
            sync_queue.pop_front();
            advanced_watermark = true;
            continue;
        }
        have_objective = TryReadMatchingObjectiveEntry(front.utc_start, objective_set);
        if (have_objective) {
            break; // ready to publish
        }
        if ((now - front.first_seen_tick) < kObjectiveGiveUpTimeoutMs) {
            break; // still within the window; wait for GWToolboxdll's own file to catch up
        }
        // No matching GWToolboxdll objective entry ever showed up - drop this run rather than publish
        // party-only data (no objective timing means it can never be leaderboard-eligible anyway).
        AppendLog(std::format("Dropping run {} (map {}): no matching objective entry after give-up timeout",
                               front.utc_start, front.map_id));
        last_persisted_utc_start = front.utc_start;
        sync_queue.pop_front();
        advanced_watermark = true;
    }
    if (advanced_watermark && !settings_folder.empty()) {
        SaveSettings(settings_folder.c_str()); // persist the advanced watermark now, not on the host's cadence
    }
    if (sync_queue.empty() || !have_objective) {
        return;
    }

    auto& next = sync_queue.front();

    // Now that we have the objective data, correct a resign/unknown classification if the run actually
    // finished (e.g. resigning right after killing Dhuum shouldn't read as giving up). Leave "wipe" as
    // reported - a genuine death event stays notable even in the rare case it's right after a kill.
    // Also corrects the local PartyLog_*.json entry, not just the published payload.
    if (next.end_reason != "wipe" && next.end_reason != "completed" && IsRunCompleted(objective_set)) {
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
        .objective = std::move(objective_set),
    };

    std::string url;
    ComposeUrl(url, kBaseUrl, kUploadRunsPath);

    publish_request = std::make_unique<AsyncRestClient>();
    publish_request->SetUrl(url.c_str());
    publish_request->SetMethod(HttpMethod::Post);
    publish_request->SetHeader("Content-Type", "application/json");
    publish_request->SetHeader("X-Machine-Key", machine_key.c_str());
    publish_request->SetHeader("X-Plugin-Version", std::to_string(kPluginVersion).c_str());
    publish_request->SetPostContent(glz::write_json(payload).value_or(std::string{}), ContentFlag::Copy);
    publish_request->SetTimeoutSec(10);
    publish_request->SetConnectTimeoutSec(5);
    publish_request->SetVerifyPeer(true);
    publish_request->SetVerifyHost(true);
    publishing_utc_start = next.utc_start;
    publish_request->ExecuteAsync();
}

// Fired once from LoadSettings, right after machine_key loads. can_report_failures defaults false
// and stays false (the safe default - failure-report logic never runs) unless/until this completes
// successfully with a true response.
void SCTracker::RequestReportPermission()
{
    can_report_failures = false;
    if (machine_key.empty()) {
        return;
    }

    std::string url;
    ComposeUrl(url, kBaseUrl, kCanReportFailurePath);

    permission_request = std::make_unique<AsyncRestClient>();
    permission_request->SetUrl(url.c_str());
    permission_request->SetMethod(HttpMethod::Get);
    permission_request->SetHeader("X-Machine-Key", machine_key.c_str());
    permission_request->SetHeader("X-Plugin-Version", std::to_string(kPluginVersion).c_str());
    permission_request->SetTimeoutSec(10);
    permission_request->SetConnectTimeoutSec(5);
    permission_request->SetVerifyPeer(true);
    permission_request->SetVerifyHost(true);
    permission_request->ExecuteAsync();
}

// Polls permission_request completion (called from Update). Any non-success outcome (network
// error, invalid/revoked key, malformed body) just leaves can_report_failures at its false default.
void SCTracker::ProcessPermissionCheck()
{
    if (!permission_request || !permission_request->IsCompleted()) {
        return;
    }
    if (permission_request->IsSuccessful()) {
        CanReportFailureResponseDto response;
        constexpr glz::opts opts{.error_on_unknown_keys = false};
        if (!glz::read<opts>(response, permission_request->GetContent())) {
            can_report_failures = response.can_report_failures;
        }
    }
    else if (permission_request->GetStatusCode() == kHttpStatusUpgradeRequired) {
        plugin_outdated = true;
        if (!version_check_request) {
            RequestLatestPluginVersion();
        }
    }
    permission_request.reset();
}

// Polls submit_request completion (called from Update, same as ProcessSync polls publish_request).
// On success the popup closes; on failure the truncated response body is kept on screen so the user
// can see why and adjust their selection before retrying.
void SCTracker::ProcessFailureSubmit()
{
    if (!submit_request || !submit_request->IsCompleted()) {
        return;
    }
    if (submit_request->IsSuccessful()) {
        show_failure_popup = false;
        failure_submit_error.clear();
    }
    else {
        if (submit_request->GetStatusCode() == kHttpStatusUpgradeRequired) {
            plugin_outdated = true;
            if (!version_check_request) {
                RequestLatestPluginVersion();
            }
        }
        std::string body = submit_request->GetContent();
        if (body.size() > 200) {
            body.resize(200);
        }
        failure_submit_error = std::format("Submit failed: status={} http_code={} body={}",
                                            submit_request->GetStatusStr(), submit_request->GetStatusCode(), body);
        AppendLog(std::format("Failure report failed for run {}: {}", pending_failure_run_id, failure_submit_error));
    }
    submit_request.reset();
}

// Fired once from LoadSettings (no machine key needed - GET /plugin-version is public) and again,
// on demand, from the 426 handlers above if a reactive check fires before this build's own copy has
// ever completed successfully - guarded by "if (!version_check_request)" at each call site so it
// never stomps one already in flight.
void SCTracker::RequestLatestPluginVersion()
{
    std::string url;
    ComposeUrl(url, kBaseUrl, kPluginVersionPath);

    version_check_request = std::make_unique<AsyncRestClient>();
    version_check_request->SetUrl(url.c_str());
    version_check_request->SetMethod(HttpMethod::Get);
    version_check_request->SetTimeoutSec(10);
    version_check_request->SetConnectTimeoutSec(5);
    version_check_request->SetVerifyPeer(true);
    version_check_request->SetVerifyHost(true);
    version_check_request->ExecuteAsync();
}

// Polls version_check_request completion (called from Update). Only ever sets plugin_outdated to
// true, never back to false within the same session - once flagged, it stays flagged until the host
// restarts the plugin with an updated build (there's no code path that clears it mid-session).
void SCTracker::ProcessVersionCheck()
{
    if (!version_check_request || !version_check_request->IsCompleted()) {
        return;
    }
    if (version_check_request->IsSuccessful()) {
        PluginVersionResponseDto response;
        constexpr glz::opts opts{.error_on_unknown_keys = false};
        if (!glz::read<opts>(response, version_check_request->GetContent())) {
            latest_known_plugin_version = response.version;
            if (kPluginVersion < response.version) {
                NotifyPluginOutdated();
            }
        }
    }
    version_check_request.reset();
}

void SCTracker::NotifyPluginOutdated()
{
    if (plugin_outdated) {
        return; // already notified this session
    }
    plugin_outdated = true;
    GW::Chat::WriteChat(GW::Chat::Channel::CHANNEL_WARNING,
                         L"<c=#FF0000>SCTracker is out of date - syncing and failure reporting are disabled "
                         "until you redownload from gwsctracker.com/account.</c>",
                         nullptr, true);
}

void SCTracker::DrawFailurePopup()
{
    // can_report_failures/plugin_outdated are re-checked here too (not just at the ProcessSync call
    // site that sets show_failure_popup) in case either changed server-side while the popup sat open.
    if (!show_failure_popup || !can_report_failures || plugin_outdated) {
        return;
    }
    ImGui::SetNextWindowSize(ImVec2(420.0f, 480.0f), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("SCTracker: Run Failure", &show_failure_popup)) {
        ImGui::TextWrapped("The most recent run ended in a wipe or resign. Which role(s) were at fault?");
        ImGui::Separator();

        for (size_t i = 0; i < kFailureReasonRoles.size(); i++) {
            // "Nobody" is mutually exclusive with every other reason: checking it clears the rest,
            // and checking any other reason clears it.
            if (ImGui::Checkbox(kFailureReasonRoles[i], &failure_role_checked[i]) && failure_role_checked[i]) {
                if (i == kNobodyReasonIndex) {
                    for (size_t j = 0; j < failure_role_checked.size(); j++) {
                        if (j != i) {
                            failure_role_checked[j] = false;
                        }
                    }
                }
                else {
                    failure_role_checked[kNobodyReasonIndex] = false;
                }
            }
        }

        if (ImGui::Button("Unselect All")) {
            failure_role_checked.fill(false);
        }

        if (!failure_submit_error.empty()) {
            ImGui::Separator();
            ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s", failure_submit_error.c_str());
        }

        ImGui::Separator();
        const bool submitting = submit_request && submit_request->IsPending();
        ImGui::BeginDisabled(submitting);
        if (ImGui::Button("Submit")) {
            ReportFailurePayload payload{.run_id = pending_failure_run_id};
            for (size_t i = 0; i < kFailureReasonRoles.size(); i++) {
                if (failure_role_checked[i]) {
                    payload.roles.emplace_back(kFailureReasonRoles[i]);
                }
            }

            std::string url;
            ComposeUrl(url, kBaseUrl, kReportFailurePath);

            submit_request = std::make_unique<AsyncRestClient>();
            submit_request->SetUrl(url.c_str());
            submit_request->SetMethod(HttpMethod::Post);
            submit_request->SetHeader("Content-Type", "application/json");
            submit_request->SetHeader("X-Machine-Key", machine_key.c_str());
            submit_request->SetHeader("X-Plugin-Version", std::to_string(kPluginVersion).c_str());
            submit_request->SetPostContent(glz::write_json(payload).value_or(std::string{}), ContentFlag::Copy);
            submit_request->SetTimeoutSec(10);
            submit_request->SetConnectTimeoutSec(5);
            submit_request->SetVerifyPeer(true);
            submit_request->SetVerifyHost(true);
            failure_submit_error.clear();
            submit_request->ExecuteAsync();
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Dismiss")) {
            show_failure_popup = false;
        }
    }
    ImGui::End();
}

void SCTracker::Draw(IDirect3DDevice9*)
{
    DrawFailurePopup();
}

void SCTracker::LoadSettings(const wchar_t* folder)
{
    ToolboxPlugin::LoadSettings(folder);
    settings_folder = folder;
    LoadSetting("machine_key", machine_key);
    LoadSetting("last_persisted_utc_start", last_persisted_utc_start);
    PluginUtils::StrCopy(machine_key_buf, machine_key.c_str(), sizeof(machine_key_buf));
    RequestLatestPluginVersion(); // no machine key needed - public endpoint, checked before anything else
    RequestReportPermission();
}

void SCTracker::SaveSettings(const wchar_t* folder)
{
    settings_folder = folder;
    SaveSetting("machine_key", machine_key);
    SaveSetting("last_persisted_utc_start", last_persisted_utc_start);
    ToolboxPlugin::SaveSettings(folder);
}

void SCTracker::DrawSettings()
{
    if (plugin_outdated) {
        if (latest_known_plugin_version > 0) {
            ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f),
                                "This SCTracker build is out of date (yours: %d, latest: %d). Syncing and "
                                "failure reporting are disabled until you redownload from gwsctracker.com/account.",
                                kPluginVersion, latest_known_plugin_version);
        }
        else {
            ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f),
                                "This SCTracker build is out of date. Syncing and failure reporting are "
                                "disabled until you redownload from gwsctracker.com/account.");
        }
        ImGui::Separator();
    }
    else if (latest_known_plugin_version > 0) {
        // Only claim up-to-date once a version check has actually succeeded (latest_known_plugin_version
        // stays 0 otherwise, e.g. the request is still in flight or failed) - see ProcessVersionCheck.
        ImGui::TextColored(ImVec4(0.4f, 0.8f, 0.4f, 1.0f), "SCTracker is up to date (version %d).", kPluginVersion);
        ImGui::Separator();
    }

    ImGui::TextWrapped("Logs party composition and run outcome for each speedclear run.");
    if (last_written_utc_start) {
        std::string time_str;
        PluginUtils::TimeToString(last_written_utc_start, time_str);
        ImGui::Text("Last run logged: %s", time_str.c_str());
    }

    ImGui::Separator();
    ImGui::TextWrapped("Syncs runs to gwsctracker.com. Leave the key blank to log locally only.");
    if (ImGui::InputText("Machine Key", machine_key_buf, sizeof(machine_key_buf), ImGuiInputTextFlags_Password)) {
        machine_key = machine_key_buf;
    }
    ImGui::Text("Sync queue: %zu pending", sync_queue.size());
    if (last_persisted_utc_start) {
        std::string time_str;
        PluginUtils::TimeToString(last_persisted_utc_start, time_str);
        ImGui::Text("Last synced run: %s", time_str.c_str());
    }
    if (!machine_key.empty()) {
        ImGui::Text("Failure reporting: %s", can_report_failures ? "enabled" : "not permitted for this key");
    }
}
