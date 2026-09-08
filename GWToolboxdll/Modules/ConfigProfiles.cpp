#include "stdafx.h"

#include <atomic>
#include <mutex>
#include <optional>

#include <GWCA/Context/CharContext.h>
#include <GWCA/Managers/MapMgr.h>
#include <GWCA/Managers/UIMgr.h>

#include <Defines.h>
#include <GWToolbox.h>
#include <Logger.h>
#include <Modules/GlobalSettings.h>
#include <Modules/GuildWarsSettingsModule.h>
#include <Modules/Resources.h>
#include <Utils/FilePersistence.h>
#include <Utils/TextUtils.h>

#include "ConfigProfiles.h"

namespace config_profiles_json {
    struct CharacterProfile {
        std::string current_profile = "default";
        std::string updated_at{};
    };

    struct CharacterProfiles {
        int version = 1;
        std::map<std::string, CharacterProfile> characters{};
    };

    struct ProfileManifest {
        int version = 1;
        std::string name{};
        std::string created_at{};
        std::string updated_at{};
        std::vector<std::string> files{};
    };
} // namespace config_profiles_json

namespace {
    constexpr auto character_profiles_filename = L"character_profiles.json";
    constexpr auto profile_manifest_filename = L"profile_manifest.json";
    constexpr auto guild_wars_settings_filename = L"GuildWarsSettings.json";
    constexpr auto legacy_guild_wars_settings_filename = L"guildwars_settings.ini";

    struct ProfileSummary {
        std::filesystem::path folder_name{};
        std::string display_name{};
    };

    enum class PendingActionType { SaveAs, SaveTarget, SaveCurrent, LogoutSave, Load, SaveGlobal, LoadGlobal, CharacterChanged, LoadGuildWarsOnly };

    struct PendingAction {
        PendingActionType type = PendingActionType::Load;
        std::filesystem::path profile{};
        std::string character{};
        std::string captured_guild_wars_settings{};
        std::string capture_warning{};
    };

    enum class StagedPhase { Idle, AwaitingGameThread, RunningOnGameThread, ReadyForRenderThread };

    struct StagedOperation {
        uint64_t id = 0;
        StagedPhase phase = StagedPhase::Idle;
        PendingAction action{};
        std::filesystem::path profile{};
        std::filesystem::path source_profile{};
        std::filesystem::path guild_wars_snapshot{};
        std::string fallback_warning{};
        std::string game_status{};
        std::string current_character{};
        bool toolbox_saved = true;
        bool game_succeeded = true;
    };

    GW::HookEntry ui_message_hook;
    config_profiles_json::CharacterProfiles character_profiles;
    std::vector<ProfileSummary> profiles;
    std::deque<PendingAction> pending_actions;
    std::mutex pending_actions_mutex;
    std::mutex staged_operation_mutex;
    StagedOperation staged_operation;
    uint64_t next_operation_id = 1;
    std::filesystem::path active_profile;
    std::string active_character;
    std::string selected_profile = "default";
    std::string status_message;
    std::vector<std::string> warnings;
    char new_profile_name[128] = {};
    std::atomic_bool initialized = false;
    bool processing = false;
    bool map_loaded_pending = false;
    std::atomic_bool initial_gw_settings_pending = false;
    std::atomic_bool logout_save_queued = false;

    std::filesystem::path ConfigsDirectory()
    {
        return Resources::GetComputerFolderPath() / L"configs";
    }

    std::filesystem::path CharacterProfilesPath()
    {
        return ConfigsDirectory() / character_profiles_filename;
    }

    std::filesystem::path NormalizeProfile(const std::filesystem::path& profile)
    {
        if (profile.empty() || _wcsicmp(profile.c_str(), L"default") == 0) return {};
        return profile.filename();
    }

    std::filesystem::path ProfileDirectory(const std::filesystem::path& profile)
    {
        const auto normalized = NormalizeProfile(profile);
        return ConfigsDirectory() / (normalized.empty() ? std::filesystem::path(L"default") : normalized);
    }

    std::string ProfileName(const std::filesystem::path& profile)
    {
        const auto normalized = NormalizeProfile(profile);
        return normalized.empty() ? "default" : normalized.string();
    }

    bool SamePath(const std::filesystem::path& left, const std::filesystem::path& right)
    {
        return _wcsicmp(left.lexically_normal().c_str(), right.lexically_normal().c_str()) == 0;
    }

    std::filesystem::path ExistingProfileName(const std::filesystem::path& profile)
    {
        if (profile.empty()) return {};
        std::error_code ec;
        for (const auto& entry : std::filesystem::directory_iterator(ConfigsDirectory(), ec)) {
            if (
                entry.is_directory(ec)
                && _wcsicmp(entry.path().filename().c_str(), profile.c_str()) == 0) {
                return entry.path().filename();
            }
        }
        return profile;
    }

    std::string CurrentCharacterName()
    {
        const auto context = GW::GetCharContext();
        return context && context->player_name[0] ? TextUtils::WStringToString(context->player_name) : std::string{};
    }

    std::string IsoTimestamp()
    {
        const auto now = std::chrono::system_clock::now();
        const auto time = std::chrono::system_clock::to_time_t(now);
        std::tm utc{};
        gmtime_s(&utc, &time);
        char buffer[32] = {};
        std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &utc);
        return buffer;
    }

    template <typename T>
    bool ReadJson(const std::filesystem::path& path, T& value)
    {
        std::string buffer;
        {
            const FilePersistence::ScopedConfigLock config_lock;
            if (!config_lock.Acquired()) return false;
            std::ifstream file(path, std::ios::binary);
            if (!file) return false;
            buffer.assign(std::istreambuf_iterator(file), {});
            if (file.bad()) return false;
        }
        auto staged = value;
        if (glz::read<glz::opts{.error_on_unknown_keys = false}>(staged, buffer)) return false;
        value = std::move(staged);
        return true;
    }

    template <typename T>
    bool WriteJsonAtomic(const std::filesystem::path& path, const T& value, std::string& error)
    {
        std::string buffer;
        if (glz::write<glz::opts{.prettify = true}>(value, buffer)) {
            error = std::format("Failed to serialise '{}'", path.filename().string());
            return false;
        }
        const FilePersistence::ScopedConfigLock config_lock;
        if (!config_lock.Acquired()) {
            error = config_lock.Error();
            return false;
        }
        return FilePersistence::AtomicWrite(path, buffer, error);
    }

    bool ReadCharacterProfiles(config_profiles_json::CharacterProfiles& loaded)
    {
        loaded = {};
        const auto path = CharacterProfilesPath();
        std::error_code ec;
        const auto file_exists = std::filesystem::exists(path, ec);
        if (ec) {
            warnings.emplace_back("Unable to inspect configs/character_profiles.json; character mappings were not changed.");
            return false;
        }
        if (!file_exists) return true;
        if (!ReadJson(path, loaded) || loaded.version != 1) {
            warnings.emplace_back("Unable to read configs/character_profiles.json; character mappings were not changed.");
            return false;
        }
        return true;
    }

    bool LoadCharacterProfiles()
    {
        config_profiles_json::CharacterProfiles loaded;
        if (!ReadCharacterProfiles(loaded)) return false;
        character_profiles = std::move(loaded);
        return true;
    }

    std::filesystem::path MappedProfile(const std::string& character_name)
    {
        if (character_name.empty()) return {};
        const auto found = character_profiles.characters.find(character_name);
        if (found == character_profiles.characters.end()) return {};
        const auto stored = std::filesystem::path(TextUtils::StringToWString(found->second.current_profile));
        if (stored.has_parent_path() || stored == L"." || stored == L"..") return {};
        return NormalizeProfile(stored);
    }

    // A config exists if it has split per-module files, a legacy single-doc json, or a legacy ini
    bool HasToolboxSettings(const std::filesystem::path& profile)
    {
        if (NormalizeProfile(profile).empty()) return true;
        const auto folder = ProfileDirectory(profile);
        std::error_code ec;
        if (!std::filesystem::is_directory(folder, ec)) return false;
        const auto modules = folder / GWTOOLBOX_MODULES_FOLDERNAME;
        if (std::filesystem::is_directory(modules, ec) && !std::filesystem::is_empty(modules, ec)) return true;
        return std::filesystem::exists(folder / GWTOOLBOX_JSON_FILENAME, ec) || std::filesystem::exists(folder / GWTOOLBOX_INI_FILENAME, ec);
    }

    std::filesystem::path SanitizeProfileName(std::string_view requested, std::string& error)
    {
        auto trimmed = TextUtils::trim(std::string(requested));
        if (trimmed.empty() || _stricmp(trimmed.c_str(), "default") == 0) return {};
        const auto sanitized = TextUtils::SanitiseFilename(trimmed);
        if (sanitized.empty() || sanitized == "." || sanitized == "..") {
            error = "Invalid profile name.";
            return {};
        }
        return std::filesystem::path(TextUtils::StringToWString(sanitized));
    }

    std::filesystem::path ResolveGuildWarsSnapshot(const std::filesystem::path& profile)
    {
        const auto directory = ProfileDirectory(profile);
        const auto json = directory / guild_wars_settings_filename;
        if (std::filesystem::exists(json)) return json;
        const auto legacy = directory / legacy_guild_wars_settings_filename;
        return std::filesystem::exists(legacy) ? legacy : std::filesystem::path{};
    }

    std::vector<std::string> EnumerateProfileFiles(const std::filesystem::path& directory)
    {
        std::vector<std::string> files;
        std::error_code ec;
        for (std::filesystem::recursive_directory_iterator it(directory, ec), end; it != end && !ec; it.increment(ec)) {
            if (!it->is_regular_file(ec)) continue;
            files.push_back(std::filesystem::relative(it->path(), directory, ec).generic_string());
        }
        std::ranges::sort(files);
        return files;
    }

    bool WriteManifest(const std::filesystem::path& profile)
    {
        const auto directory = ProfileDirectory(profile);
        const auto path = directory / profile_manifest_filename;
        config_profiles_json::ProfileManifest manifest;
        if (std::filesystem::exists(path)) ReadJson(path, manifest);
        if (manifest.created_at.empty()) manifest.created_at = IsoTimestamp();
        manifest.version = 1;
        manifest.name = ProfileName(profile);
        manifest.updated_at = IsoTimestamp();
        manifest.files = EnumerateProfileFiles(directory);
        const auto manifest_relative = std::filesystem::path(profile_manifest_filename).generic_string();
        if (!std::ranges::contains(manifest.files, manifest_relative)) {
            manifest.files.push_back(manifest_relative);
            std::ranges::sort(manifest.files);
        }
        std::string error;
        if (WriteJsonAtomic(path, manifest, error)) return true;
        warnings.push_back(error);
        Log::Error("%s", error.c_str());
        return false;
    }

    void QueuePendingAction(PendingAction action)
    {
        const std::scoped_lock lock(pending_actions_mutex);
        pending_actions.push_back(std::move(action));
    }

    std::optional<PendingAction> PopPendingAction()
    {
        const std::scoped_lock lock(pending_actions_mutex);
        if (pending_actions.empty()) return std::nullopt;
        auto action = std::move(pending_actions.front());
        pending_actions.pop_front();
        return action;
    }

    void StageForGameThread(StagedOperation operation)
    {
        operation.id = next_operation_id++;
        operation.phase = StagedPhase::AwaitingGameThread;
        const std::scoped_lock lock(staged_operation_mutex);
        staged_operation = std::move(operation);
    }

    std::optional<StagedOperation> TakeCompletedGameOperation()
    {
        const std::scoped_lock lock(staged_operation_mutex);
        if (staged_operation.phase != StagedPhase::ReadyForRenderThread) return std::nullopt;
        auto operation = std::move(staged_operation);
        staged_operation = {};
        return operation;
    }

    bool SaveGuildWarsSnapshot(const std::filesystem::path& profile, std::string& status)
    {
        return GuildWarsSettingsModule::SaveCurrentSettingsToFile(ProfileDirectory(profile) / guild_wars_settings_filename, status);
    }

    bool ShouldSkipCopiedPath(const std::filesystem::path& relative)
    {
        if (relative.empty()) return true;
        const auto first = relative.begin()->wstring();
        if (_wcsicmp(first.c_str(), L"backups") == 0) return true;
        const auto filename = relative.filename().wstring();
        return _wcsicmp(filename.c_str(), profile_manifest_filename) == 0
            || filename.ends_with(L".tmp")
            || filename.find(L".tmp.") != std::wstring::npos
            || filename.starts_with(L"performance_log");
    }

    bool CopyProfileFiles(const std::filesystem::path& source, const std::filesystem::path& destination)
    {
        if (SamePath(source, destination) || !std::filesystem::exists(source)) return true;
        const FilePersistence::ScopedConfigLock config_lock(10000);
        if (!config_lock.Acquired()) {
            warnings.push_back(config_lock.Error());
            return false;
        }
        std::error_code ec;
        std::filesystem::create_directories(destination, ec);
        for (std::filesystem::recursive_directory_iterator it(source, ec), end; it != end && !ec; it.increment(ec)) {
            const auto relative = std::filesystem::relative(it->path(), source, ec);
            if (ec) break;
            if (ShouldSkipCopiedPath(relative)) {
                if (it->is_directory(ec)) it.disable_recursion_pending();
                continue;
            }
            const auto target = destination / relative;
            if (it->is_directory(ec)) {
                std::filesystem::create_directories(target, ec);
            }
            else if (it->is_regular_file(ec)) {
                std::filesystem::create_directories(target.parent_path(), ec);
                if (ec) break;
                std::ifstream file(it->path(), std::ios::binary);
                const auto buffer = std::string(std::istreambuf_iterator(file), {});
                if (!file || file.bad()) {
                    warnings.push_back(std::format("Failed to read profile file '{}'", it->path().string()));
                    return false;
                }
                std::string error;
                if (!FilePersistence::AtomicWrite(target, buffer, error)) {
                    warnings.push_back(std::move(error));
                    return false;
                }
            }
            if (ec) break;
        }
        if (!ec) return true;
        warnings.push_back(std::format("Failed to copy profile files: {}", ec.message()));
        return false;
    }

    void BindCharacter(const std::string& character, const std::filesystem::path& profile)
    {
        if (character.empty()) return;
        const FilePersistence::ScopedConfigLock config_lock;
        if (!config_lock.Acquired()) {
            warnings.push_back(config_lock.Error());
            return;
        }
        // Reload under the write lock so another client cannot lose its newly added mapping.
        config_profiles_json::CharacterProfiles updated;
        if (!ReadCharacterProfiles(updated)) return;
        const auto profile_name = ProfileName(profile);
        const auto existing = updated.characters.find(character);
        if (
            existing != updated.characters.end()
            && _stricmp(existing->second.current_profile.c_str(), profile_name.c_str()) == 0) {
            character_profiles = std::move(updated);
            active_character = character;
            return;
        }
        updated.characters[character] = {.current_profile = profile_name, .updated_at = IsoTimestamp()};
        std::string error;
        if (!WriteJsonAtomic(CharacterProfilesPath(), updated, error)) {
            warnings.push_back(error);
            Log::Error("%s", error.c_str());
            return;
        }
        character_profiles = std::move(updated);
        active_character = character;
    }

    void RefreshProfiles()
    {
        profiles.clear();
        profiles.push_back({{}, "default"});
        std::error_code ec;
        std::filesystem::create_directories(ConfigsDirectory(), ec);
        for (const auto& entry : std::filesystem::directory_iterator(ConfigsDirectory(), ec)) {
            if (!entry.is_directory(ec) || _wcsicmp(entry.path().filename().c_str(), L"default") == 0) continue;
            ProfileSummary summary{entry.path().filename(), entry.path().filename().string()};
            config_profiles_json::ProfileManifest manifest;
            if (ReadJson(entry.path() / profile_manifest_filename, manifest) && !manifest.name.empty()) summary.display_name = manifest.name;
            profiles.push_back(std::move(summary));
        }
        std::ranges::sort(profiles.begin() + 1, profiles.end(), [](const auto& left, const auto& right) {
            return _stricmp(left.display_name.c_str(), right.display_name.c_str()) < 0;
        });
    }

    bool LoadGuildWarsSnapshot(const std::filesystem::path& snapshot, std::string& status)
    {
        if (snapshot.empty()) return true;
        return GuildWarsSettingsModule::LoadSettingsFromFile(snapshot, status);
    }

    void FinishRenderFailure(const std::string& message)
    {
        status_message = message;
        Log::Error("%s", status_message.c_str());
        processing = false;
    }

    void BeginLoadProfile(PendingAction action, std::filesystem::path profile, std::string fallback_warning = {})
    {
        profile = ExistingProfileName(NormalizeProfile(profile));
        const auto previous_profile = NormalizeProfile(Resources::GetSettingsFolderName());
        status_message = std::format("Loading profile '{}'...", ProfileName(profile));
        if (!HasToolboxSettings(profile)) {
            FinishRenderFailure(std::format("Profile '{}' has no Toolbox settings; load cancelled.", ProfileName(profile)));
            if (action.type == PendingActionType::CharacterChanged) active_character = action.character;
            return;
        }
        warnings.clear();
        if (!GWToolbox::SetSettingsFolder(profile)) {
            FinishRenderFailure(std::format("Unable to open profile '{}'.", ProfileName(profile)));
            if (action.type == PendingActionType::CharacterChanged) active_character = action.character;
            return;
        }
        if (GWToolbox::LoadSettings().empty()) {
            const auto restored = GWToolbox::SetSettingsFolder(previous_profile);
            const auto previous_settings_restored = restored && !GWToolbox::LoadSettings().empty();
            active_profile = NormalizeProfile(Resources::GetSettingsFolderName());
            FinishRenderFailure(previous_settings_restored
                ? std::format("Unable to load profile '{}'; restored profile '{}'.", ProfileName(profile), ProfileName(previous_profile))
                : std::format("Unable to load profile '{}' and could not restore profile '{}'.", ProfileName(profile), ProfileName(previous_profile)));
            if (action.type == PendingActionType::CharacterChanged) active_character = action.character;
            return;
        }
        StagedOperation operation;
        operation.action = std::move(action);
        operation.profile = std::move(profile);
        operation.guild_wars_snapshot = ResolveGuildWarsSnapshot(operation.profile);
        operation.fallback_warning = std::move(fallback_warning);
        StageForGameThread(std::move(operation));
    }

    void BeginSaveCurrent(PendingAction action)
    {
        warnings.clear();
        const auto profile = NormalizeProfile(Resources::GetSettingsFolderName());
        status_message = std::format("Saving profile '{}'...", ProfileName(profile));
        StagedOperation operation;
        operation.action = std::move(action);
        operation.profile = profile;
        operation.toolbox_saved = !GWToolbox::SaveSettings().empty();
        StageForGameThread(std::move(operation));
    }

    void BeginLogoutSave(PendingAction action)
    {
        warnings.clear();
        const auto profile = NormalizeProfile(action.profile);
        status_message = std::format("Saving profile '{}' after logout...", ProfileName(profile));

        auto toolbox_saved = false;
        if (SamePath(profile, NormalizeProfile(Resources::GetSettingsFolderName()))) {
            toolbox_saved = !GWToolbox::SaveSettings().empty();
        }
        else {
            warnings.emplace_back("The active profile changed before its logout save could run; Toolbox settings were not written to the wrong profile.");
        }

        std::string game_status;
        const auto game_saved = !action.captured_guild_wars_settings.empty()
            && GuildWarsSettingsModule::SaveCapturedSettingsToFile(
                ProfileDirectory(profile) / guild_wars_settings_filename,
                action.captured_guild_wars_settings,
                game_status);
        if (!action.capture_warning.empty()) warnings.push_back(std::move(action.capture_warning));
        if (!game_saved && !game_status.empty()) warnings.push_back(game_status);
        const auto manifest_saved = WriteManifest(profile);
        const auto saved = toolbox_saved && game_saved && manifest_saved;
        status_message = std::format("Saved profile '{}' after logout{}.", ProfileName(profile), saved ? "" : " with warnings");
        if (saved) Log::Info("%s", status_message.c_str());
        else Log::Warning("%s", status_message.c_str());
        processing = false;
        logout_save_queued = false;
        RefreshProfiles();
    }

    void BeginSaveTarget(PendingAction action)
    {
        const auto profile = ExistingProfileName(NormalizeProfile(action.profile));
        const auto source_profile = NormalizeProfile(Resources::GetSettingsFolderName());
        status_message = std::format("Saving profile '{}' without changing the active profile...", ProfileName(profile));
        if (SamePath(profile, source_profile)) {
            action.type = PendingActionType::SaveCurrent;
            BeginSaveCurrent(std::move(action));
            return;
        }
        warnings.clear();
        if (!CopyProfileFiles(ProfileDirectory(source_profile), ProfileDirectory(profile))) {
            FinishRenderFailure(std::format("Unable to copy settings into profile '{}'.", ProfileName(profile)));
            return;
        }
        if (!GWToolbox::SetSettingsFolder(profile)) {
            active_profile = NormalizeProfile(Resources::GetSettingsFolderName());
            FinishRenderFailure(std::format("Unable to open profile '{}' for saving.", ProfileName(profile)));
            return;
        }
        const auto toolbox_saved = !GWToolbox::SaveSettings().empty();
        const auto restored = GWToolbox::SetSettingsFolder(source_profile);
        active_profile = NormalizeProfile(Resources::GetSettingsFolderName());
        if (!restored) {
            FinishRenderFailure(std::format(
                "Saved profile '{}', but could not restore active profile '{}'.",
                ProfileName(profile),
                ProfileName(source_profile)));
            RefreshProfiles();
            return;
        }
        StagedOperation operation;
        operation.action = std::move(action);
        operation.profile = profile;
        operation.source_profile = source_profile;
        operation.toolbox_saved = toolbox_saved;
        StageForGameThread(std::move(operation));
    }

    void BeginSaveAs(PendingAction action)
    {
        const auto profile = ExistingProfileName(NormalizeProfile(action.profile));
        const auto source_profile = NormalizeProfile(Resources::GetSettingsFolderName());
        status_message = std::format("Saving profile '{}'...", ProfileName(profile));
        warnings.clear();
        StagedOperation operation;
        operation.action = std::move(action);
        operation.profile = profile;
        operation.source_profile = source_profile;
        operation.toolbox_saved = !GWToolbox::SaveSettings().empty();
        StageForGameThread(std::move(operation));
    }

    void BeginLoadGuildWarsOnly(PendingAction action)
    {
        warnings.clear();
        initial_gw_settings_pending = false;
        StagedOperation operation;
        operation.action = std::move(action);
        operation.profile = active_profile;
        operation.guild_wars_snapshot = ResolveGuildWarsSnapshot(active_profile);
        if (operation.guild_wars_snapshot.empty()) {
            processing = false;
            return;
        }
        StageForGameThread(std::move(operation));
    }

    void BeginGlobalAction(const PendingActionType type)
    {
        std::string error;
        const auto saving = type == PendingActionType::SaveGlobal;
        const auto succeeded = saving ? GlobalSettings::Save(&error) : GlobalSettings::Load(&error);
        status_message = succeeded
            ? std::format("Global settings {}.", saving ? "saved" : "loaded")
            : std::format("Unable to {} global settings: {}", saving ? "save" : "load", error);
        if (succeeded) Log::Info("%s", status_message.c_str());
        else Log::Error("%s", status_message.c_str());
        processing = false;
    }

    void BeginCharacterChanged(PendingAction action)
    {
        if (action.character.empty()) {
            processing = false;
            return;
        }
        if (action.character == active_character) {
            if (!initial_gw_settings_pending.exchange(false)) {
                processing = false;
                return;
            }
            action.type = PendingActionType::LoadGuildWarsOnly;
            BeginLoadGuildWarsOnly(std::move(action));
            return;
        }
        auto target = MappedProfile(action.character);
        std::string fallback_warning;
        if (!HasToolboxSettings(target)) {
            fallback_warning = std::format("Profile '{}' mapped to '{}' is missing; using default.", ProfileName(target), action.character);
            Log::Warning("%s", fallback_warning.c_str());
            target.clear();
        }
        if (SamePath(target, NormalizeProfile(Resources::GetSettingsFolderName()))) {
            // Keep live settings across shared-profile character swaps, including unsaved changes.
            const auto needs_initial_settings = active_character.empty() || initial_gw_settings_pending.exchange(false);
            active_character = action.character;
            if (needs_initial_settings) {
                action.type = PendingActionType::LoadGuildWarsOnly;
                BeginLoadGuildWarsOnly(std::move(action));
            }
            else {
                processing = false;
            }
            if (!fallback_warning.empty()) warnings.push_back(std::move(fallback_warning));
            return;
        }
        initial_gw_settings_pending = false;
        action.profile = target;
        BeginLoadProfile(std::move(action), target, std::move(fallback_warning));
    }

    void RecordGameFailure(const StagedOperation& operation, const bool saving)
    {
        if (operation.game_succeeded || operation.game_status.empty()) return;
        warnings.push_back(operation.game_status);
        if (saving) Log::Error("%s", operation.game_status.c_str());
        else Log::Warning("%s", operation.game_status.c_str());
    }

    void FinalizeSaveCurrent(const StagedOperation& operation)
    {
        RecordGameFailure(operation, true);
        const auto manifest_saved = WriteManifest(operation.profile);
        active_profile = operation.profile;
        const auto saved = operation.toolbox_saved && operation.game_succeeded && manifest_saved;
        status_message = std::format("Saved profile '{}'{}.", ProfileName(operation.profile), saved ? "" : " with warnings");
        if (saved) Log::Info("%s", status_message.c_str());
        else Log::Warning("%s", status_message.c_str());
        processing = false;
        RefreshProfiles();
    }

    void FinalizeSaveTarget(const StagedOperation& operation)
    {
        RecordGameFailure(operation, true);
        const auto manifest_saved = WriteManifest(operation.profile);
        const auto saved = operation.toolbox_saved && operation.game_succeeded && manifest_saved;
        status_message = std::format(
            "Saved profile '{}' without changing the active profile{}.",
            ProfileName(operation.profile),
            saved ? "" : " (with warnings)");
        if (saved) Log::Info("%s", status_message.c_str());
        else Log::Warning("%s", status_message.c_str());
        processing = false;
        RefreshProfiles();
    }

    void FinalizeSaveAs(const StagedOperation& operation)
    {
        RecordGameFailure(operation, true);
        const auto source_manifest_saved = WriteManifest(operation.source_profile);
        if (!(operation.toolbox_saved && operation.game_succeeded && source_manifest_saved)) {
            status_message = std::format("Unable to save active profile '{}' before Save As.", ProfileName(operation.source_profile));
            Log::Warning("%s", status_message.c_str());
            processing = false;
            RefreshProfiles();
            return;
        }
        warnings.clear();
        if (!CopyProfileFiles(ProfileDirectory(operation.source_profile), ProfileDirectory(operation.profile))) {
            FinishRenderFailure(std::format("Unable to copy settings into profile '{}'.", ProfileName(operation.profile)));
            return;
        }
        if (!GWToolbox::SetSettingsFolder(operation.profile)) {
            FinishRenderFailure(std::format("Unable to open profile '{}'.", ProfileName(operation.profile)));
            return;
        }
        const auto toolbox_saved = !GWToolbox::SaveSettings().empty();
        const auto manifest_saved = WriteManifest(operation.profile);
        active_profile = operation.profile;
        BindCharacter(operation.current_character, operation.profile);
        selected_profile = ProfileName(operation.profile);
        const auto saved = toolbox_saved && manifest_saved;
        status_message = std::format("Saved and selected profile '{}'{}.", ProfileName(operation.profile), saved ? "" : " with warnings");
        if (saved) Log::Info("%s", status_message.c_str());
        else Log::Warning("%s", status_message.c_str());
        processing = false;
        RefreshProfiles();
    }

    void FinalizeLoad(const StagedOperation& operation)
    {
        RecordGameFailure(operation, false);
        active_profile = operation.profile;
        if (operation.action.type == PendingActionType::Load) {
            BindCharacter(operation.current_character, operation.profile);
        }
        else {
            active_character = operation.action.character;
        }
        selected_profile = ProfileName(operation.profile);
        status_message = std::format("Loaded profile '{}'.", ProfileName(operation.profile));
        Log::Info("%s", status_message.c_str());
        if (!operation.fallback_warning.empty()) warnings.push_back(operation.fallback_warning);
        processing = false;
        RefreshProfiles();
    }

    void FinalizeGameOperation(StagedOperation operation)
    {
        switch (operation.action.type) {
            case PendingActionType::SaveAs:
                FinalizeSaveAs(operation);
                break;
            case PendingActionType::SaveTarget:
                FinalizeSaveTarget(operation);
                break;
            case PendingActionType::SaveCurrent:
                FinalizeSaveCurrent(operation);
                break;
            case PendingActionType::LogoutSave:
                processing = false;
                break;
            case PendingActionType::Load:
            case PendingActionType::CharacterChanged:
                FinalizeLoad(operation);
                break;
            case PendingActionType::LoadGuildWarsOnly:
                RecordGameFailure(operation, false);
                processing = false;
                break;
            case PendingActionType::SaveGlobal:
            case PendingActionType::LoadGlobal:
                processing = false;
                break;
        }
    }

    void OnUiMessage(GW::HookStatus*, const GW::UI::UIMessage message_id, void* wparam, void*)
    {
        if (!initialized) return;
        if (message_id == GW::UI::UIMessage::kLogout) {
            map_loaded_pending = false;
            initial_gw_settings_pending = false;
            const auto logout = static_cast<GW::UI::UIPacket::kLogout*>(wparam);
            if (logout && logout->character_select == 1 && !logout_save_queued.exchange(true)) {
                PendingAction action;
                action.type = PendingActionType::LogoutSave;
                action.profile = NormalizeProfile(Resources::GetSettingsFolderName());
                action.character = active_character;
                try {
                    GuildWarsSettingsModule::CaptureCurrentSettings(action.captured_guild_wars_settings, action.capture_warning);
                }
                catch (const std::exception& exception) {
                    action.capture_warning = std::format("Capture failed: {}", exception.what());
                }
                catch (...) {
                    action.capture_warning = "Capture failed with an unknown error";
                }
                if (action.captured_guild_wars_settings.empty()) {
                    Log::Warning("Unable to capture Guild Wars settings before logout: %s", action.capture_warning.c_str());
                }
                QueuePendingAction(std::move(action));
            }
        }
        else if (message_id == GW::UI::UIMessage::kMapLoaded) {
            map_loaded_pending = true;
        }
    }
} // namespace

void ConfigProfiles::PrepareInitialProfile()
{
    warnings.clear();
    LoadCharacterProfiles();
    active_character = CurrentCharacterName();
    active_profile = MappedProfile(active_character);
    if (!HasToolboxSettings(active_profile)) {
        Log::Warning("Mapped profile '%s' is missing; using default", ProfileName(active_profile).c_str());
        active_profile.clear();
    }
    GWToolbox::SetSettingsFolder(active_profile);
    selected_profile = ProfileName(active_profile);
    initial_gw_settings_pending = !active_character.empty();
}

void ConfigProfiles::Initialize()
{
    if (initialized) return;
    initialized = true;
    RefreshProfiles();
    RegisterUIMessageCallback(&ui_message_hook, GW::UI::UIMessage::kLogout, OnUiMessage);
    RegisterUIMessageCallback(&ui_message_hook, GW::UI::UIMessage::kMapLoaded, OnUiMessage, 0x8000);
    map_loaded_pending = true;
}

void ConfigProfiles::ProcessPendingActions()
{
    if (!initialized) return;
    if (auto completed = TakeCompletedGameOperation()) {
        FinalizeGameOperation(std::move(*completed));
        return;
    }
    if (processing) return;
    auto pending = PopPendingAction();
    if (!pending) return;
    processing = true;
    switch (pending->type) {
        case PendingActionType::SaveAs:
            BeginSaveAs(std::move(*pending));
            break;
        case PendingActionType::SaveTarget:
            BeginSaveTarget(std::move(*pending));
            break;
        case PendingActionType::SaveCurrent:
            BeginSaveCurrent(std::move(*pending));
            break;
        case PendingActionType::LogoutSave:
            BeginLogoutSave(std::move(*pending));
            break;
        case PendingActionType::Load:
            {
                const auto profile = pending->profile;
                BeginLoadProfile(std::move(*pending), profile);
            }
            break;
        case PendingActionType::SaveGlobal:
        case PendingActionType::LoadGlobal:
            BeginGlobalAction(pending->type);
            break;
        case PendingActionType::CharacterChanged:
            BeginCharacterChanged(std::move(*pending));
            break;
        case PendingActionType::LoadGuildWarsOnly:
            BeginLoadGuildWarsOnly(std::move(*pending));
            break;
    }
}

void ConfigProfiles::ProcessGameThreadActions()
{
    if (!initialized) return;

    StagedOperation operation;
    bool run_operation = false;
    {
        const std::scoped_lock lock(staged_operation_mutex);
        if (staged_operation.phase == StagedPhase::AwaitingGameThread) {
            staged_operation.phase = StagedPhase::RunningOnGameThread;
            operation = staged_operation;
            run_operation = true;
        }
    }
    if (run_operation) {
        std::string game_status;
        bool game_succeeded = true;
        switch (operation.action.type) {
            case PendingActionType::SaveAs:
                game_succeeded = SaveGuildWarsSnapshot(operation.source_profile, game_status);
                break;
            case PendingActionType::SaveTarget:
            case PendingActionType::SaveCurrent:
                game_succeeded = SaveGuildWarsSnapshot(operation.profile, game_status);
                break;
            case PendingActionType::LogoutSave:
                break;
            case PendingActionType::Load:
            case PendingActionType::CharacterChanged:
            case PendingActionType::LoadGuildWarsOnly:
                game_succeeded = LoadGuildWarsSnapshot(operation.guild_wars_snapshot, game_status);
                break;
            case PendingActionType::SaveGlobal:
            case PendingActionType::LoadGlobal:
                break;
        }
        const auto character = CurrentCharacterName();
        const std::scoped_lock lock(staged_operation_mutex);
        if (
            staged_operation.id == operation.id
            && staged_operation.phase == StagedPhase::RunningOnGameThread) {
            staged_operation.game_succeeded = game_succeeded;
            staged_operation.game_status = std::move(game_status);
            staged_operation.current_character = character;
            staged_operation.phase = StagedPhase::ReadyForRenderThread;
        }
    }

    if (!map_loaded_pending || !GW::Map::GetIsMapLoaded()) return;
    const auto character = CurrentCharacterName();
    if (character.empty()) return;
    map_loaded_pending = false;
    QueuePendingAction({PendingActionType::CharacterChanged, {}, character});
}

void ConfigProfiles::Terminate()
{
    if (!initialized) return;
    GW::UI::RemoveUIMessageCallback(&ui_message_hook);
    initialized = false;
    {
        const std::scoped_lock lock(pending_actions_mutex);
        pending_actions.clear();
    }
    {
        const std::scoped_lock lock(staged_operation_mutex);
        staged_operation = {};
    }
    processing = false;
    logout_save_queued = false;
}

bool ConfigProfiles::QueueSaveAsProfile(const std::string_view profile_name, std::string* error)
{
    std::string local_error;
    const auto profile = SanitizeProfileName(profile_name, local_error);
    if (!local_error.empty()) {
        if (error) *error = local_error;
        return false;
    }
    QueuePendingAction({PendingActionType::SaveAs, profile});
    return true;
}

bool ConfigProfiles::QueueSaveProfile(const std::string_view profile_name, std::string* error)
{
    std::string local_error;
    const auto profile = SanitizeProfileName(profile_name, local_error);
    if (!local_error.empty()) {
        if (error) *error = local_error;
        return false;
    }
    QueuePendingAction({PendingActionType::SaveTarget, profile});
    return true;
}

bool ConfigProfiles::QueueSaveCurrentProfile(std::string*)
{
    QueuePendingAction({PendingActionType::SaveCurrent});
    return true;
}

bool ConfigProfiles::QueueLoadProfile(const std::string_view profile_name, std::string* error)
{
    std::string local_error;
    const auto profile = SanitizeProfileName(profile_name, local_error);
    if (!local_error.empty()) {
        if (error) *error = local_error;
        return false;
    }
    QueuePendingAction({PendingActionType::Load, profile});
    return true;
}

bool ConfigProfiles::QueueSaveGlobalSettings(std::string*)
{
    QueuePendingAction({PendingActionType::SaveGlobal});
    return true;
}

bool ConfigProfiles::QueueLoadGlobalSettings(std::string*)
{
    QueuePendingAction({PendingActionType::LoadGlobal});
    return true;
}

void ConfigProfiles::DrawSettings()
{
    if (!ImGui::TreeNodeEx("Config Profiles", ImGuiTreeNodeFlags_FramePadding | ImGuiTreeNodeFlags_SpanAvailWidth)) return;
    const auto& character = active_character;
    ImGui::Text("Current character: %s", character.empty() ? "(none)" : character.c_str());
    ImGui::Text("Active profile: %s", ProfileName(Resources::GetSettingsFolderName()).c_str());
    ImGui::TextDisabled("Profiles switch automatically by character; unassigned characters use default.");

    ImGui::InputText("New profile name", new_profile_name, _countof(new_profile_name));
    if (ImGui::Button("Save As Profile")) {
        if (TextUtils::trim(new_profile_name).empty()) {
            status_message = "Enter a profile name before using Save As.";
        }
        else {
            std::string error;
            if (!QueueSaveAsProfile(new_profile_name, &error)) status_message = error;
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Save Current Profile")) QueueSaveCurrentProfile();

    if (ImGui::BeginCombo("Profile", selected_profile.c_str())) {
        for (const auto& profile : profiles) {
            const auto name = ProfileName(profile.folder_name);
            const auto selected = selected_profile == name;
            const auto label = profile.display_name == name ? name : std::format("{} ({})", profile.display_name, name);
            if (ImGui::Selectable(label.c_str(), selected)) selected_profile = name;
            if (selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    if (ImGui::Button("Load Selected Profile")) {
        std::string error;
        if (!QueueLoadProfile(selected_profile, &error)) status_message = error;
    }
    ImGui::SameLine();
    if (ImGui::Button("Refresh Profiles")) {
        LoadCharacterProfiles();
        RefreshProfiles();
        status_message = "Profiles refreshed.";
    }

    if (!status_message.empty()) ImGui::TextWrapped("%s", status_message.c_str());
    if (!warnings.empty() && ImGui::TreeNode("Profile warnings")) {
        for (const auto& warning : warnings)
            ImGui::BulletText("%s", warning.c_str());
        ImGui::TreePop();
    }
    ImGui::TreePop();
}
