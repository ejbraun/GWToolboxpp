#pragma once

#include <ToolboxUIPlugin.h>

#include <IconsFontAwesome5.h>
#include <GWCA/GameContainers/GamePos.h>
#include <GWCA/Utilities/Hook.h>

#include <array>
#include <chrono>
#include <mutex>
#include <optional>

class PitsSoulsWindow : public ToolboxUIPlugin {
public:
    PitsSoulsWindow()
    {
        can_show_in_main_window = true;
    }
    ~PitsSoulsWindow() override = default;

    const char* Name() const override { return "PitsSouls"; }
    const char* Icon() const override { return ICON_FA_GHOST; }

    void Update(float) override;
    void DrawSettings() override;
    void Draw(IDirect3DDevice9* pDevice) override;

    void Initialize(ImGuiContext* ctx, ImGuiAllocFns fns, HMODULE toolbox_dll) override;
    void SignalTerminate() override;

private:
    using Clock = std::chrono::steady_clock;
    struct PitsSoul {
        enum class State { Alive, Dead, Unknown };
        GW::Vec2f pos;
        const char* name;
        uint32_t agent_id = 0;
        State currentState = State::Unknown;
        std::optional<float> readHpPercent;
        std::optional<Clock::time_point> expectedDeathTime;
        double secondsRemaining = 0;
        bool observedDead = false;

        void observe(float hp, bool dead, Clock::time_point now);
        void observeDeath(Clock::time_point now);
        void update(Clock::time_point now);
        std::string print() const;
    };

    void resetSouls();
    PitsSoul* findSoul(GW::Vec2f pos);
    void bindSoul(PitsSoul& soul, uint32_t agent_id);

    std::mutex state_mutex;
    std::array<PitsSoul, 3> souls{};
    bool underworld_instance = false;
    bool terminating = false;
    uint32_t last_instance_time = 0;
    GW::HookEntry transfer_hook;
    GW::HookEntry instance_hook;
    GW::HookEntry spawn_hook;
    GW::HookEntry despawn_hook;
    GW::HookEntry state_hook;
};
