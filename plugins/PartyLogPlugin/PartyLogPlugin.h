#pragma once

#include <ToolboxPlugin.h>
#include <PluginUtils.h>

#include <GWCA/Utilities/Hook.h>

#include <memory>
#include <string>
#include <vector>

// Logs the party's composition (players/heroes/henchmen + professions) whenever the player enters
// an explorable area, keyed by UTC start time so it can be joined against GWToolboxdll's own
// runs/ObjectiveTimerRuns_*.json files without needing any changes to GWToolboxdll itself.
class PartyLogPlugin : public ToolboxPlugin {
public:
    PartyLogPlugin() = default;
    ~PartyLogPlugin() override = default;

    const char* Name() const override { return "Party Log Plugin"; }

    [[nodiscard]] bool HasSettings() const override { return true; }
    void DrawSettings() override;

    void Initialize(ImGuiContext* ctx, ImGuiAllocFns allocator_fns, HMODULE toolbox_dll) override;
    void Terminate() override;
    void Update(float delta) override;

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
    void CaptureParty();
    void WriteLogEntry();

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

    uint32_t last_written_utc_start = 0; // for DrawSettings status display only

    GW::HookEntry InstanceLoadInfo_HookEntry;
};
