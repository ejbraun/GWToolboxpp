#pragma once

#include <string>

class SettingsDoc;
class ToolboxIni;

class GlobalSettings final {
public:
    static bool EnsureModulesLoaded(SettingsDoc& profile, ToolboxIni* legacy, std::string* error = nullptr);
    static bool EnsurePluginsLoaded(SettingsDoc& profile, ToolboxIni* legacy, std::string* error = nullptr);
    static bool EnsureHotkeysLoaded(SettingsDoc& profile, ToolboxIni* legacy, std::string* error = nullptr);

    static bool Save(std::string* error = nullptr);
    static bool Load(std::string* error = nullptr);
    static bool SaveModules(std::string* error = nullptr);
    static bool SavePlugins(std::string* error = nullptr);
    static bool SaveHotkeys(std::string* error = nullptr);
    static void StripProfilePayloads(SettingsDoc& profile);

    static void InvalidatePlugins();
    static void InvalidateHotkeys();
};
