#pragma once

#include <ToolboxUIPlugin.h>

#include <IconsFontAwesome5.h>
#include <GWCA/Utilities/Hook.h>

#include <chrono>
#include <deque>
#include <optional>
#include <mutex>

class DhuumCalculator : public ToolboxUIPlugin {
public:
    DhuumCalculator() { can_show_in_main_window = true; }
    ~DhuumCalculator() override = default;

    const char* Name() const override { return "DhuumCalculator"; }
    const char* Icon() const override { return ICON_FA_GHOST; }

    void Initialize(ImGuiContext* ctx, ImGuiAllocFns allocator_fns, HMODULE toolbox_dll) override;
    void SignalTerminate() override;
    void Update(float) override;
    void DrawSettings() override;
    void Draw(IDirect3DDevice9* pDevice) override;
private:
    struct DhuumStatus {
        float hp;
        float rest;
        std::chrono::steady_clock::time_point time;
    };

    void resetPredictions();
    void resetEncounter();
    void OnMissionProgress(uint8_t id, float filled, bool created);

    // Packet hooks and rendering can run on different threads.
    std::mutex state_mutex;
    std::deque<DhuumStatus> history;
    std::deque<int64_t> damagePredictions;
    std::deque<int64_t> restPredictions;
    std::deque<int64_t> missingDamagePredictions;
    int64_t damageFinishPrediction = 0;
    int64_t restFinishPrediction = 0;
    int64_t missingDamagePrediction = 0;
    bool predictions_ready = false;

    uint32_t dhuum_agent_id = 0;
    uint32_t last_instance_time = 0;
    std::chrono::steady_clock::time_point next_agent_search{};
    std::optional<uint8_t> progress_id;
    std::optional<float> rest_progress;
    float health = 0.f;
    uint32_t max_health = 0;
    bool has_dhuum = false;
    bool underworld_instance = false;
    bool terminating = false;

    GW::HookEntry transfer_hook;
    GW::HookEntry instance_hook;
    GW::HookEntry agent_add_hook;
    GW::HookEntry agent_remove_hook;
    GW::HookEntry progress_create_hook;
    GW::HookEntry progress_update_hook;
};
