#pragma once

#include <ToolboxPlugin.h>
#include <PluginUtils.h>

#include <GWCA/Utilities/Hook.h>

// unique_ptr<AsyncRestClient> below needs the complete type: ~PartyLogPlugin() is defaulted inline in
// this header, so unique_ptr's deleter is instantiated here too, not just where AsyncRestClient is used.
#include <RestClient.h>

#include <deque>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

// Logs the party's composition (players/heroes/henchmen + professions) and how each explorable-area
// run ended (wipe/resign/unknown), keyed by UTC start time so it can be joined against GWToolboxdll's
// own runs/ObjectiveTimerRuns_*.json files without needing any changes to GWToolboxdll itself.
//
// Also syncs both to a configurable backend endpoint: a periodic sweep (SyncQueueEntry) reads the
// local PartyLog_*.json / ObjectiveTimerRuns_*.json files - the durable source of truth, written
// regardless of network state - for anything past last_persisted_utc_start, and publishes oldest
// first, only advancing the watermark on confirmed success. This means a slow GWToolboxdll write, a
// network blip, or the game closing mid-publish just gets retried/caught up on a later sweep instead
// of being lost.
class PartyLogPlugin : public ToolboxPlugin {
public:
    PartyLogPlugin() = default;
    ~PartyLogPlugin() override = default;

    const char* Name() const override { return "Party Log Plugin"; }

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
    };

private:
    void OnInstanceLoadInfo(uint32_t map_id, bool is_explorable);
    void OnGameSrvTransfer();
    void OnPartyDefeated();
    void OnWriteToChatLog(const wchar_t* message);
    void CaptureParty();
    void WriteLogEntry(const std::string& end_reason);

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

    // --- Backend sync ---
    void ProcessSync();
    void RefreshSyncQueue();

    std::string endpoint_url;
    std::string machine_key;
    char endpoint_url_buf[256] = "";
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
