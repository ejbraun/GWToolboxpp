#pragma once

#include <ToolboxModule.h>

#include <filesystem>
#include <string_view>

class GuildWarsSettingsModule : public ToolboxModule {
    GuildWarsSettingsModule() = default;
    ~GuildWarsSettingsModule() override = default;

public:
    static GuildWarsSettingsModule& Instance()
    {
        static GuildWarsSettingsModule instance;
        return instance;
    }

    [[nodiscard]] const char* Icon() const override { return ICON_FA_CHECK_SQUARE; }
    [[nodiscard]] const char* Name() const override { return "Guild Wars Settings"; }
    [[nodiscard]] const char* Description() const override { return "Ability to save or load Guild Wars settings to a file on disk"; }

    void Initialize() override;
    void Terminate() override;
    void DrawSettingsInternal() override;

    // These access live client preferences and must run on the game thread.
    static bool CaptureCurrentSettings(std::string& serialized, std::string& status);
    static bool SaveCurrentSettingsToFile(const std::filesystem::path& path, std::string& status);
    static bool LoadSettingsFromFile(const std::filesystem::path& path, std::string& status);

    // Captured settings contain no live client pointers and can be written from the render thread.
    static bool SaveCapturedSettingsToFile(const std::filesystem::path& path, std::string_view serialized, std::string& status);
};
