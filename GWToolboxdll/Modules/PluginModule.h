#pragma once

#include <ToolboxUIElement.h>
#include <Utils/GwrlProtocol.h>
#include <atomic>
#include <mutex>
#include <../plugins/Base/ToolboxPlugin.h>

class PluginModule final : public ToolboxUIElement {
    PluginModule() = default;
    ~PluginModule() override = default;

public:
    struct Plugin {
        Plugin(std::filesystem::path _path)
            : path(std::move(_path)) { }

        std::filesystem::path path;
        HMODULE dll = nullptr;
        ToolboxPlugin* instance = nullptr;
        bool initialized = false;
        bool terminating = false;
        bool visible = false;
        bool enabled = false;
        bool reserved = false;
        bool resume_loaded = false;
        HMODULE released_module = nullptr;
        uint32_t version = 0;
        uint32_t abi = 0;
        std::string sha256;
        std::shared_ptr<std::atomic_bool> stop_barrier;

    };

    static PluginModule& Instance()
    {
        static PluginModule instance;
        return instance;
    }

    [[nodiscard]] const char* Name() const override { return "Plugins"; }
    [[nodiscard]] const char* Icon() const override { return ICON_FA_PUZZLE_PIECE; }

    [[nodiscard]] bool ShowOnWorldMap() const override { return true; }

    void Draw(IDirect3DDevice9*) override;
    void DrawSettingsInternal() override;
    void LoadSettings(SettingsDoc& doc, ToolboxIni* legacy) override;
    void SaveSettings(SettingsDoc& doc) override;
    void Update(float) override;
    void Initialize() override;
    void SignalTerminate() override;
    void Terminate() override;
    bool CanTerminate() override;
    bool WndProc(UINT, WPARAM, LPARAM) override;

    static std::unique_lock<std::recursive_mutex> AcquireLock();
    static std::vector<ToolboxPlugin*> GetPlugins();
    static std::vector<Gwrl::Artifact> Inventory(bool refresh = false);
    static bool ReserveUpdate(const std::vector<std::string>& names, bool all, std::string& error);
    static void StartUpdateUnload();
    static bool UpdateReleased();
    static bool RestoreUpdate(const std::vector<Gwrl::Artifact>& expected, std::string& error);
    static bool UpdateRestored();
    static bool StartupComplete(std::string& error);
    static void CancelReservation();


    void ShowVisibleRadio() override { }
    void DrawSizeAndPositionSettings() override { }
};
