#pragma once

#include <ToolboxPlugin.h>
#include <PluginUtils.h>

#include <GWCA/Utilities/Hook.h>

// unique_ptr<AsyncRestClient> below needs the complete type: ~SCTracker() is defaulted inline in
// this header, so unique_ptr's deleter is instantiated here too, not just where AsyncRestClient is used.
#include <RestClient.h>

#include <array>
#include <deque>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace GW::Constants {
    enum class SkillID : uint32_t;
}

// Client-side data collection for a GW1 speedclear run tracker (Go backend, Postgres, React frontend):
// this plugin is the thing that actually runs inside the game and feeds that backend. GWToolboxdll's
// own Objective Timer already records per-run timing/objective data (runs/ObjectiveTimerRuns_*.json),
// but has no concept of party composition or how a run ended - this plugin fills that gap without
// requiring any changes to GWToolboxdll itself:
//
//   - Captures who was in the party (players/heroes/henchmen + professions) and how each tracked
//     explorable-area run ended (wipe/resign/completed/unknown), keyed by UTC start time so it lines up with
//     GWToolboxdll's own ObjectiveTimerRuns_*.json entries for the same run.
//   - Only for instances GWToolboxdll's ObjectiveTimerWindow actually tracks (kTrackedMapIds) - random
//     missions/vanquishes/etc. are skipped entirely, since they'd never correlate with anything.
//   - Periodically (SyncQueueEntry) reads both its own local PartyLog_*.json and GWToolboxdll's
//     ObjectiveTimerRuns_*.json - the durable, network-independent source of truth - and publishes the
//     combined party+objective payload for each run to the backend, machine-key authenticated. Only
//     advances the persisted watermark on confirmed success, so a slow GWToolboxdll write, a network
//     blip, or the game closing mid-publish just gets retried/caught up on a later sweep instead of
//     losing data or double-reporting a run.
class SCTracker : public ToolboxPlugin {
public:
    SCTracker() = default;
    ~SCTracker() override = default;

    const char* Name() const override { return "SCTracker"; }

    [[nodiscard]] bool HasSettings() const override { return true; }
    void DrawSettings() override;
    void LoadSettings(const wchar_t* folder) override;
    void SaveSettings(const wchar_t* folder) override;

    void Initialize(ImGuiContext* ctx, ImGuiAllocFns allocator_fns, HMODULE toolbox_dll) override;
    void Terminate() override;
    void Update(float delta) override;
    // Renders the post-run failure-report popup only (show_failure_popup gates it) - everything
    // else about this plugin is background data collection with no always-on window.
    void Draw(IDirect3DDevice9*) override;
    // Destroying an in-flight publish_request/submit_request blocks (joins the background HTTP
    // thread); deferring unload until both are done avoids freezing the host UI on plugin disable.
    bool CanTerminate() override
    {
        return (!publish_request || publish_request->IsCompleted())
            && (!submit_request || submit_request->IsCompleted())
            && (!permission_request || permission_request->IsCompleted())
            && (!version_check_request || version_check_request->IsCompleted());
    }

    struct PartyMember {
        std::string name;
        uint32_t primary = 0;   // GW::Constants::Profession
        uint32_t secondary = 0; // GW::Constants::Profession
        bool is_player = false;
        bool is_hero = false;
        bool is_henchman = false;
        uint32_t deaths = 0; // count of alive->dead transitions seen for this member during the run
        // English names of every kTrackedSkillNameSet skill the LOCAL PLAYER (this run's uploader) has
        // used at least once during the run, if they're themselves Ranger/Assassin (2/7) - see
        // OnSkillUsed. Always empty on every other party member's entry: tracking anyone else's skill
        // usage this way is unreliable (only ever observed when they're within compass range of the
        // uploader), so the client doesn't attempt it at all. Deduped.
        std::vector<std::string> role_skills;
        // "t1"/"t2"/"t3" once role_skills satisfies one of kRoleCombos, else "unknown". Set once and
        // never overwritten afterward. Like role_skills, only ever non-"unknown" on the uploader's own
        // entry - this is the uploader's own role, self-determined from their own skill usage, never a
        // guess about someone else.
        std::string role_hint = "unknown";
        struct ItemDropCount {
            uint32_t id = 0; // model_id (GW::Constants::ItemID) of a kTrackedItems entry
            uint32_t count = 0;
        };
        // One entry per kTrackedItems model_id reserved for this member at least once during the run.
        // Reflects initial loot reservation (GAME_SMSG_ITEM_UPDATE_OWNER), not confirmed pickup - see
        // OnItemUpdateOwner.
        std::vector<ItemDropCount> item_drops;
    };

private:
    // Only tracks instances GWToolboxdll's own ObjectiveTimerWindow would create an ObjectiveSet for
    // (see kTrackedMapIds) - skips capture/hooks entirely for everything else (random missions,
    // vanquishes, etc.), so the sync queue never fills up with entries that can never find a matching
    // objective log. DoA is deliberately excluded: it's not in ObjectiveTimerWindow's map_id switch at
    // all (it's gated on a different packet's map_fileID, since DoA shares its map_id with the solo
    // Mallyx mission) - out of scope here per explicit instruction.
    void OnInstanceLoadInfo(uint32_t map_id, bool is_explorable);
    void OnGameSrvTransfer();
    void OnPartyDefeated();
    void OnWriteToChatLog(const wchar_t* message);
    void OnUpdateAgentState(uint32_t agent_id, uint32_t state);
    void OnAgentUpdateAllegiance(uint32_t agent_id, uint32_t allegiance_bits);
    void OnObjectiveDone(uint32_t objective_id);
    void OnSkillUsed(uint32_t agent_id, GW::Constants::SkillID skill_id);
    void FlushPendingRoleSkills();
    void ProcessTrackedSkillUse(const std::string& skill_name);
    void OnItemGeneral(uint32_t item_id, uint32_t model_id);
    void OnItemUpdateOwner(uint32_t item_id, uint32_t owner_agent_id);
    void CaptureParty();
    void WriteLogEntry(uint32_t utc_start, uint32_t map_id, const std::string& character_name,
                        const std::string& end_reason, const std::vector<PartyMember>& members);

    // PluginUtils::EncString has no safe way to detach from a pending GW::UI::AsyncDecodeStr callback
    // before destruction (unlike GWToolboxdll's internal GuiUtils::EncString, which has AbandonDecode()).
    // So a new run's capture can only start once every in-flight EncString from the previous one has
    // finished decoding — restart_requested + next_* stage the new run until that's safe.
    bool restart_requested = false;
    uint32_t next_utc_start = 0;
    uint32_t next_map_id = 0;
    std::string next_character_name;

    bool active_capture = false;
    uint32_t pending_utc_start = 0;
    uint32_t pending_map_id = 0;
    std::string pending_character_name;
    std::vector<PartyMember> party_members;
    std::vector<std::unique_ptr<PluginUtils::EncString>> party_member_enc_names;

    // Death tracking for the in-progress run: agent_id -> index into party_members, populated as each
    // member is captured (agent_id is known at that point even before capture fully completes). Not
    // serialized itself - only the resulting PartyMember::deaths counts are. currently_dead is parallel
    // to party_members and used purely to detect the alive->dead edge (AgentState can repeat/re-send the
    // same bit), so a later resurrection doesn't get double-counted and a second death after that does.
    std::unordered_map<uint32_t, size_t> agent_id_to_party_index;
    std::vector<bool> party_member_currently_dead;

    // skill_id -> decoded English name, populated lazily (once per distinct skill_id the local player
    // ever uses, not per cast) so role tracking matches by name instead of the numeric SkillID enum
    // ordinal, which shifts whenever GWCA's Skills.h header gains/loses an entry anywhere earlier in
    // the list. Persists across runs deliberately (skill names don't change mid-session) rather than
    // being reset in OnInstanceLoadInfo.
    std::unordered_map<uint32_t, std::unique_ptr<PluginUtils::EncString>> skill_name_cache;
    struct PendingRoleSkillEvent {
        uint32_t skill_id = 0; // key into skill_name_cache
    };
    // OnSkillUsed calls for a skill_id whose name hadn't finished decoding yet when it fired; drained
    // by FlushPendingRoleSkills (called from Update) once its cache lookup is ready. Always about the
    // local player - OnSkillUsed only ever queues an event after confirming that.
    std::vector<PendingRoleSkillEvent> pending_role_skill_events;

    // item_id -> model_id for tracked-item drops seen via ItemGeneral but not yet resolved to an
    // owner. Erased once OnItemUpdateOwner counts it (or the owner isn't a tracked party member), so a
    // later reservation reassignment for the same item_id isn't double-counted. Reset every run.
    std::unordered_map<uint32_t, uint32_t> tracked_item_id_to_model_id;

    // Run outcome tracking: reset on every run start (OnInstanceLoadInfo), finalized and logged when the
    // run ends (OnGameSrvTransfer). The actual write is deferred to run-end rather than capture-completion
    // so the log entry can record how the run finished.
    bool run_active = false;
    bool wipe_detected = false;
    std::unordered_set<uint32_t> resigned_login_numbers;
    // Set once Dhuum's agent spawns hostile (see OnAgentUpdateAllegiance) and left set for the rest
    // of the run. Deaths after this point are deliberate/expected (e.g. the Dhuum tank) rather than
    // run-ending mistakes, so OnUpdateAgentState stops incrementing PartyMember::deaths once this is set.
    bool dhuum_started = false;
    // Set once the native Dhuum mission-objective completes (see OnObjectiveDone) and left set for the
    // rest of the run. OnItemGeneral stops counting Glob of Ectoplasm drops into item_drops once this
    // is set - other tracked items are unaffected.
    bool dhuum_completed = false;

    uint32_t last_written_utc_start = 0; // for DrawSettings status display only

    GW::HookEntry InstanceLoadInfo_HookEntry;
    GW::HookEntry GameSrvTransfer_HookEntry;
    GW::HookEntry PartyDefeated_HookEntry;
    GW::HookEntry WriteToChatLog_HookEntry;
    GW::HookEntry AgentState_HookEntry;
    GW::HookEntry AgentUpdateAllegiance_HookEntry;
    GW::HookEntry GenericValueSelf_HookEntry;
    GW::HookEntry GenericValueTarget_HookEntry;
    GW::HookEntry ItemGeneral_HookEntry;
    GW::HookEntry ItemUpdateOwner_HookEntry;
    GW::HookEntry ObjectiveDone_HookEntry;

    // --- Backend sync ---
    void ProcessSync();
    void RefreshSyncQueue();

    std::string machine_key;
    char machine_key_buf[128] = "";

    uint32_t last_persisted_utc_start = 0; // persisted setting; watermark, only advances on confirmed publish
    std::wstring settings_folder;          // cached from LoadSettings/SaveSettings so a successful publish
                                            // can persist the advanced watermark immediately, not just on
                                            // whatever cadence the host calls SaveSettings.

    struct SyncQueueEntry {
        uint32_t utc_start = 0;
        uint32_t map_id = 0;
        std::string character_name;
        std::string end_reason;
        std::vector<PartyMember> party_members;
        uint64_t first_seen_tick = 0; // GetTickCount64() when first queued; for the give-up-waiting timeout
    };
    std::deque<SyncQueueEntry> sync_queue;
    std::unique_ptr<AsyncRestClient> publish_request;
    uint32_t publishing_utc_start = 0; // utc_start of the entry publish_request is currently sending
    uint64_t last_queue_scan_tick = 0;
    uint64_t last_publish_attempt_tick = 0; // backoff timer, only advanced on a failed publish

    // --- Post-run failure reporting ---
    // Popup opens automatically once a run that ended in wipe/resign finishes publishing and its
    // server-assigned run_id is known (see ProcessSync). The checkbox list itself is a static,
    // hardcoded role vocabulary (kFailureReasonRoles in the .cpp) - the plugin has no way to know
    // which character actually held which role in a given run (that's derived server-side); the
    // backend rejects any role not actually present in the run and this shows that error inline.
    //
    // can_report_failures gates all of it (queried once, right after machine_key loads - see
    // RequestReportPermission/LoadSettings): defaults false, and none of the rest of this section's
    // logic runs at all until the server confirms permission - not just "hidden," genuinely skipped.
    void RequestReportPermission();  // fires permission_request; called once from LoadSettings
    void ProcessPermissionCheck();   // polls permission_request completion; called from Update
    void DrawFailurePopup();
    void ProcessFailureSubmit(); // polls submit_request completion; called from Update

    bool can_report_failures = false;
    std::unique_ptr<AsyncRestClient> permission_request;

    bool show_failure_popup = false;
    int64_t pending_failure_run_id = 0;
    std::array<bool, 11> failure_role_checked{}; // parallel to kFailureReasonRoles
    std::string failure_submit_error;            // non-empty renders as an inline error in the popup
    std::unique_ptr<AsyncRestClient> submit_request;

    // --- Plugin version check ---
    // Two complementary mechanisms, both driven by the same plugin_outdated flag: proactively,
    // RequestLatestPluginVersion (fired once from LoadSettings, no machine key needed - public
    // endpoint) compares the server's declared latest version against this build's own kPluginVersion
    // constant before any sync/report attempt is even made. Reactively, every machine-key-
    // authenticated request (publish_request/submit_request/permission_request) now also sends its
    // own X-Plugin-Version header, and a 426 response from any of them (see their respective
    // completion handlers) sets plugin_outdated too - a backstop for the case where this build was
    // current when the proactive check ran but a newer one has shipped since. Once true,
    // plugin_outdated disables ProcessSync's publish attempt and the failure-report popup entirely
    // (not just a warning - see their respective gates) until the plugin is updated and restarted.
    void RequestLatestPluginVersion(); // fires version_check_request; called once from LoadSettings
    void ProcessVersionCheck();        // polls version_check_request completion; called from Update

    bool plugin_outdated = false;
    int latest_known_plugin_version = 0; // 0 until a successful check has actually reported one
    std::unique_ptr<AsyncRestClient> version_check_request;
};
