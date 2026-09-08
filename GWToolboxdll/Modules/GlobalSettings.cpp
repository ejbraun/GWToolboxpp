#include "stdafx.h"

#include <atomic>
#include <filesystem>
#include <map>
#include <mutex>
#include <vector>

#include <GWToolbox.h>
#include <Modules/PluginModule.h>
#include <Modules/Resources.h>
#include <Modules/ToolboxSettings.h>
#include <ToolboxIni.h>
#include <Utils/SettingsDoc.h>
#include <Windows/HotkeysWindow.h>

#include "GlobalSettings.h"

namespace {
    constexpr auto global_folder = L"global";
    constexpr auto modules_filename = L"modules.json";
    constexpr auto plugins_filename = L"plugins.json";
    constexpr auto hotkeys_filename = L"hotkeys.json";

    std::recursive_mutex global_settings_mutex;
    std::atomic_bool modules_loaded = false;
    std::atomic_bool plugins_loaded = false;
    std::atomic_bool hotkeys_loaded = false;

    std::filesystem::path GlobalPath(const std::filesystem::path& filename)
    {
        return Resources::GetPath(global_folder, filename);
    }

    void SetError(std::string* out, std::string message)
    {
        if (out) *out = std::move(message);
    }

    template <typename HasSettings, typename ApplySettings, typename CaptureSettings>
    bool EnsureDomainLoaded(
        const char* domain, const std::filesystem::path& path, SettingsDoc& profile, ToolboxIni* legacy, const bool force, std::atomic_bool& loaded, HasSettings&& has_settings, ApplySettings&& apply_settings, CaptureSettings&& capture_settings,
        std::string* error
    )
    {
        if (loaded.load(std::memory_order_acquire) && !force) return true;
        const std::scoped_lock lock(global_settings_mutex);
        if (loaded.load(std::memory_order_acquire) && !force) return true;

        std::error_code filesystem_error;
        const auto exists = std::filesystem::exists(path, filesystem_error);
        if (filesystem_error) {
            SetError(error, std::format("Unable to inspect global {} settings '{}': {}", domain, path.string(), filesystem_error.message()));
            return false;
        }

        SettingsDoc global;
        if (exists && !global.LoadFile(path)) {
            SetError(error, std::format("Unable to read global {} settings from '{}'", domain, path.string()));
            return false;
        }

        const auto migrate = !exists || !has_settings(global);
        auto& source = migrate ? profile : global;
        if (!apply_settings(source, migrate ? legacy : nullptr)) {
            SetError(error, std::format("Unable to apply global {} settings", domain));
            return false;
        }

        if (migrate) {
            SettingsDoc migrated;
            capture_settings(migrated);
            if (!migrated.SaveFile(path)) {
                SetError(error, std::format("Unable to migrate global {} settings to '{}'", domain, path.string()));
                return false;
            }
        }

        loaded.store(true, std::memory_order_release);
        return true;
    }

    template <typename CaptureSettings>
    bool SaveDomain(const char* domain, const std::filesystem::path& path, std::atomic_bool& loaded, CaptureSettings&& capture_settings, std::string* error)
    {
        const std::scoped_lock lock(global_settings_mutex);
        SettingsDoc global;
        capture_settings(global);
        if (!global.SaveFile(path)) {
            SetError(error, std::format("Unable to save global {} settings to '{}'", domain, path.string()));
            return false;
        }
        loaded.store(true, std::memory_order_release);
        return true;
    }

    void AppendError(std::string& combined, const std::string& next)
    {
        if (next.empty()) return;
        if (!combined.empty()) combined += "; ";
        combined += next;
    }
} // namespace

bool GlobalSettings::EnsureModulesLoaded(SettingsDoc& profile, ToolboxIni* legacy, std::string* error)
{
    return EnsureDomainLoaded(
        "module", GlobalPath(modules_filename), profile, legacy, false, modules_loaded,
        [](const SettingsDoc& doc) {
            return doc.HasSection("Toolbox Modules");
        },
        [](SettingsDoc& doc, ToolboxIni* ini) {
            return ToolboxSettings::LoadGlobalSettings(doc, ini);
        },
        [](SettingsDoc& doc) {
            ToolboxSettings::SaveGlobalSettings(doc);
        },
        error
    );
}

bool GlobalSettings::EnsurePluginsLoaded(SettingsDoc& profile, ToolboxIni* legacy, std::string* error)
{
    return EnsureDomainLoaded(
        "plugin", GlobalPath(plugins_filename), profile, legacy, false, plugins_loaded,
        [](const SettingsDoc& doc) {
            return doc.Has("Plugins", "enabled_plugins");
        },
        [](SettingsDoc& doc, ToolboxIni* ini) {
            return PluginModule::Instance().LoadGlobalSettings(doc, ini);
        },
        [](SettingsDoc& doc) {
            PluginModule::Instance().SaveGlobalSettings(doc);
        },
        error
    );
}

bool GlobalSettings::EnsureHotkeysLoaded(SettingsDoc& profile, ToolboxIni* legacy, std::string* error)
{
    return EnsureDomainLoaded(
        "hotkey", GlobalPath(hotkeys_filename), profile, legacy, false, hotkeys_loaded,
        [](const SettingsDoc& doc) {
            return doc.Has("Hotkeys", "hotkeys");
        },
        [](SettingsDoc& doc, ToolboxIni* ini) {
            return HotkeysWindow::Instance().LoadGlobalSettings(doc, ini);
        },
        [](SettingsDoc& doc) {
            HotkeysWindow::Instance().SaveGlobalSettings(doc);
        },
        error
    );
}

bool GlobalSettings::SaveModules(std::string* error)
{
    return SaveDomain(
        "module", GlobalPath(modules_filename), modules_loaded,
        [](SettingsDoc& doc) {
            ToolboxSettings::SaveGlobalSettings(doc);
        },
        error
    );
}

bool GlobalSettings::SavePlugins(std::string* error)
{
    const std::scoped_lock lock(global_settings_mutex);
    if (!plugins_loaded.load(std::memory_order_acquire)) return true;
    return SaveDomain(
        "plugin", GlobalPath(plugins_filename), plugins_loaded,
        [](SettingsDoc& doc) {
            PluginModule::Instance().SaveGlobalSettings(doc);
        },
        error
    );
}

bool GlobalSettings::SaveHotkeys(std::string* error)
{
    const std::scoped_lock lock(global_settings_mutex);
    if (!hotkeys_loaded.load(std::memory_order_acquire)) return true;
    return SaveDomain(
        "hotkey", GlobalPath(hotkeys_filename), hotkeys_loaded,
        [](SettingsDoc& doc) {
            HotkeysWindow::Instance().SaveGlobalSettings(doc);
        },
        error
    );
}

bool GlobalSettings::Save(std::string* error)
{
    std::string combined;
    std::string domain_error;
    auto success = SaveModules(&domain_error);
    AppendError(combined, domain_error);
    domain_error.clear();
    success = SavePlugins(&domain_error) && success;
    AppendError(combined, domain_error);
    domain_error.clear();
    success = SaveHotkeys(&domain_error) && success;
    AppendError(combined, domain_error);
    SetError(error, std::move(combined));
    return success;
}

bool GlobalSettings::Load(std::string* error)
{
    const auto profile = GWToolbox::GetSettingsDoc();
    const auto legacy = GWToolbox::OpenSettingsFile();
    if (!profile || !legacy) {
        SetError(error, "The active profile is not available for global settings migration");
        return false;
    }

    const auto plugin_was_enabled = GWToolbox::IsModuleEnabled(&PluginModule::Instance());
    const auto hotkeys_were_enabled = GWToolbox::IsModuleEnabled(&HotkeysWindow::Instance());

    std::string combined;
    std::string domain_error;
    auto success = EnsureDomainLoaded(
        "module", GlobalPath(modules_filename), *profile, legacy, true, modules_loaded,
        [](const SettingsDoc& doc) {
            return doc.HasSection("Toolbox Modules");
        },
        [](SettingsDoc& doc, ToolboxIni* ini) {
            return ToolboxSettings::LoadGlobalSettings(doc, ini);
        },
        [](SettingsDoc& doc) {
            ToolboxSettings::SaveGlobalSettings(doc);
        },
        &domain_error
    );
    AppendError(combined, domain_error);
    if (success) ToolboxSettings::LoadModules(legacy);

    if (GWToolbox::IsModuleEnabled(&PluginModule::Instance()) && plugin_was_enabled) {
        domain_error.clear();
        success = EnsureDomainLoaded(
                      "plugin", GlobalPath(plugins_filename), *profile, legacy, true, plugins_loaded,
                      [](const SettingsDoc& doc) {
                          return doc.Has("Plugins", "enabled_plugins");
                      },
                      [](SettingsDoc& doc, ToolboxIni* ini) {
                          return PluginModule::Instance().LoadGlobalSettings(doc, ini);
                      },
                      [](SettingsDoc& doc) {
                          PluginModule::Instance().SaveGlobalSettings(doc);
                      },
                      &domain_error
                  ) &&
                  success;
        AppendError(combined, domain_error);
    }
    if (GWToolbox::IsModuleEnabled(&HotkeysWindow::Instance()) && hotkeys_were_enabled) {
        domain_error.clear();
        success = EnsureDomainLoaded(
                      "hotkey", GlobalPath(hotkeys_filename), *profile, legacy, true, hotkeys_loaded,
                      [](const SettingsDoc& doc) {
                          return doc.Has("Hotkeys", "hotkeys");
                      },
                      [](SettingsDoc& doc, ToolboxIni* ini) {
                          return HotkeysWindow::Instance().LoadGlobalSettings(doc, ini);
                      },
                      [](SettingsDoc& doc) {
                          HotkeysWindow::Instance().SaveGlobalSettings(doc);
                      },
                      &domain_error
                  ) &&
                  success;
        AppendError(combined, domain_error);
    }

    SetError(error, std::move(combined));
    return success;
}

void GlobalSettings::StripProfilePayloads(SettingsDoc& profile)
{
    const auto persisted = [](const bool loaded, const std::filesystem::path& path, const auto& has_settings) {
        if (loaded) return true;
        SettingsDoc global;
        return global.LoadFile(path) && has_settings(global);
    };
    if (persisted(modules_loaded.load(std::memory_order_acquire), GlobalPath(modules_filename), [](const SettingsDoc& doc) {
            std::map<std::string, bool> values;
            return doc.GetStruct("Toolbox Modules", values);
        })) {
        ToolboxSettings::RemoveGlobalSettings(profile);
    }
    if (persisted(plugins_loaded.load(std::memory_order_acquire), GlobalPath(plugins_filename), [](const SettingsDoc& doc) {
            std::vector<std::string> values;
            return doc.Get("Plugins", "enabled_plugins", values);
        })) {
        profile.EraseKey("Plugins", "enabled_plugins");
    }
    if (persisted(hotkeys_loaded.load(std::memory_order_acquire), GlobalPath(hotkeys_filename), [](const SettingsDoc& doc) {
            std::vector<HotkeysWindow::HotkeyEntry> values;
            return doc.Get("Hotkeys", "hotkeys", values);
        })) {
        profile.EraseKey("Hotkeys", "hotkeys");
    }
}

void GlobalSettings::InvalidatePlugins()
{
    const std::scoped_lock lock(global_settings_mutex);
    plugins_loaded.store(false, std::memory_order_release);
}

void GlobalSettings::InvalidateHotkeys()
{
    const std::scoped_lock lock(global_settings_mutex);
    hotkeys_loaded.store(false, std::memory_order_release);
}
