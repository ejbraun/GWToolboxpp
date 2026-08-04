#pragma once

#include <ToolboxPlugin.h>
#include <PluginUtils.h>

#include <GWCA/Utilities/Hook.h>

// unique_ptr<AsyncRestClient> below needs the complete type: ~SCTracker() is defaulted inline in
// this header, so unique_ptr's deleter is instantiated here too, not just where AsyncRestClient is used.
#include <RestClient.h>

#include <deque>
#include <memory>
#include <optional>
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
    // Destroying an in-flight publish_request blocks (joins the background HTTP thread); deferring
    // unload until it's done avoids freezing the host UI on plugin disable.
    bool CanTerminate() override { return !publish_request || publish_request->IsCompleted(); }

    struct PartyMember {
        std::string name;
        uint32_t primary = 0;   // GW::Constants::Profession
        uint32_t secondary = 0; // GW::Constants::Profession
        bool is_player = false;
        bool is_hero = false;
        bool is_henchman = false;
        uint32_t deaths = 0; // count of alive->dead transitions seen for this member during the run
        // Set for Ranger/Assassin members (primary profession only) the first time they use one of
        // kRoleSkills' mapped skills; never overwritten afterward. Absent for everyone else.
        std::optional<std::string> role_hint;
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
    void OnSkillUsed(uint32_t agent_id, GW::Constants::SkillID skill_id);
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

    // Run outcome tracking: reset on every run start (OnInstanceLoadInfo), finalized and logged when the
    // run ends (OnGameSrvTransfer). The actual write is deferred to run-end rather than capture-completion
    // so the log entry can record how the run finished.
    bool run_active = false;
    bool wipe_detected = false;
    std::unordered_set<uint32_t> resigned_login_numbers;

    uint32_t last_written_utc_start = 0; // for DrawSettings status display only

    GW::HookEntry InstanceLoadInfo_HookEntry;
    GW::HookEntry GameSrvTransfer_HookEntry;
    GW::HookEntry PartyDefeated_HookEntry;
    GW::HookEntry WriteToChatLog_HookEntry;
    GW::HookEntry AgentState_HookEntry;
    GW::HookEntry GenericValueSelf_HookEntry;
    GW::HookEntry GenericValueTarget_HookEntry;

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
};
