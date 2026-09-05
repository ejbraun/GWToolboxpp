#include "stdafx.h"
#include <ForkVersion.h>
#include <Utils/GwrlTransport.h>
#include <Modules/CrashHandler.h>
#include <GWCA/Managers/GameThreadMgr.h>
#include <../plugins/Base/ArtifactMetadata.h>

#include "PluginModule.h"
#include "../plugins/Base/ToolboxPlugin.h"

#include <GWToolbox.h>
#include <GWCA/Managers/ChatMgr.h>

#include <Defender.h>
#include <Modules/Resources.h>
#include <filesystem>
#include <string>

#include "GWCA/Managers/UIMgr.h"
#include "Utils/TextUtils.h"

namespace PluginMetadataJson { struct Manifest { uint32_t version = 0; std::string sha256; }; }

namespace {
    std::recursive_mutex plugin_mutex;
    bool update_reserved = false;
    bool startup_settings_loaded = false;
    std::vector<PluginModule::Plugin*> startup_plugins;
    std::wstring pluginsfoldername;

    const char* plugins_enabled_section = "Plugins Enabled";

    std::vector<PluginModule::Plugin*> plugins_available;

    std::vector<PluginModule::Plugin*> plugins_loaded;

    bool UnloadPlugin(PluginModule::Plugin* plugin_ptr, const bool preserve_enabled = false)
    {
        auto& plugin = *plugin_ptr;
        if (!preserve_enabled && !plugin.reserved) plugin.enabled = false;
        if (!plugin.terminating) {
            if (plugin.initialized && plugin.instance) {
                plugin.instance->SaveSettings(pluginsfoldername.c_str());
                plugin.instance->SignalTerminate();
            }
            if (!plugin.dll && !plugin.instance && !plugin.released_module) return true;
            if (!std::ranges::contains(plugins_loaded, plugin_ptr)) plugins_loaded.push_back(plugin_ptr);
            plugin.stop_barrier = std::make_shared<std::atomic_bool>(false);
            GW::GameThread::Enqueue([barrier = plugin.stop_barrier] { barrier->store(true); });
            plugin.terminating = true;
        }
        if (!plugin.stop_barrier->load() || (plugin.instance && !plugin.instance->CanTerminate())) return false; // Pending
        if (plugin.instance && plugin.initialized) plugin.instance->Terminate();
        plugin.initialized = false;
        plugin.instance = nullptr;
        if (plugin.dll) {
            const auto module = plugin.dll;
            if (!FreeLibrary(module)) return false;
            plugin.released_module = module;
            plugin.dll = nullptr;
        }
        if (plugin.released_module) {
            HMODULE still_loaded = nullptr;
            if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCWSTR>(plugin.released_module), &still_loaded)) return false;
            plugin.released_module = nullptr;
        }
        plugin.terminating = false;
        plugin.stop_barrier.reset();
        std::erase(plugins_loaded, plugin_ptr);
        return true;
    }

    bool LoadPlugin(PluginModule::Plugin* plugin_ptr, const bool restoring = false)
    {
        auto& plugin = *plugin_ptr;
        if ((!restoring && (update_reserved || plugin.reserved)) || plugin.terminating) return false;
        if (plugin.instance) return true;
        if (!plugin.dll) {
            plugin.dll = LoadLibraryW(plugin.path.wstring().c_str());
        }
        if (!plugin.dll) {
            const DWORD err = GetLastError();
            const auto filename = plugin.path.filename();
            UnloadPlugin(plugin_ptr);
            const auto name = TextUtils::PrintFilename(filename.wstring());
            std::wstring detail;
            if ((err == ERROR_VIRUS_INFECTED || err == ERROR_VIRUS_DELETED) && FindRecentDefenderBlock(filename.wstring(), 15, detail))
                Log::ErrorW(L"Failed to load plugin %s - Windows Defender blocked it: %s", name.c_str(), detail.c_str());
            else
                Log::ErrorW(L"Failed to load plugin %s (LoadLibraryW)", name.c_str());
            return false;
        }
        using ToolboxPluginInstanceFn = ToolboxPlugin* (*)();
        const auto instance_fn = reinterpret_cast<ToolboxPluginInstanceFn>(GetProcAddress(plugin.dll, "ToolboxPluginInstance"));
        if (!instance_fn) {
            UnloadPlugin(plugin_ptr);
            Log::Error("Failed to load plugin %s (ToolboxPluginInstance)", TextUtils::PrintFilename(plugin.path.filename().string()).c_str());
            return false;
        }

        plugin.instance = instance_fn();
        if (!plugin.instance) {
            UnloadPlugin(plugin_ptr);
            return false;
        }
        plugin.enabled = true;
        plugin.version = 0;
        plugin.abi = 0;
        plugin.sha256 = Gwrl::FileSha256(plugin.path);
        if (const auto metadata = reinterpret_cast<ToolboxArtifactMetadataFn>(GetProcAddress(plugin.dll, "ToolboxArtifactInfo"))) {
            if (const auto info = metadata(); info && info->size == sizeof(ToolboxArtifactMetadata)) {
                plugin.version = info->version;
                plugin.abi = info->abi;
            }
        }
        if (plugin.path.filename() == "DBBox.dll" && plugin.abi != GWTOOLBOX_PLUGIN_ABI) {
            UnloadPlugin(plugin_ptr, true);
            Log::Error("DBBox requires a matching Toolbox plugin ABI.");
            return false;
        }
        if (!plugin.version) {
            auto manifest_path = plugin.path;
            manifest_path.replace_extension(".version.json");
            std::string json;
            std::error_code file_error;
            if (std::filesystem::file_size(manifest_path, file_error) <= Gwrl::MaximumPayload && !file_error) {
                std::ifstream manifest_file(manifest_path);
                json.assign(std::istreambuf_iterator<char>(manifest_file), {});
            }
            PluginMetadataJson::Manifest manifest;
            if (json.size() <= Gwrl::MaximumPayload && !glz::read<glz::opts{.error_on_unknown_keys = false}>(manifest, json)
                && manifest.sha256 == plugin.sha256) plugin.version = manifest.version;
        }
        if (!std::ranges::contains(plugins_loaded, plugin_ptr)) plugins_loaded.push_back(plugin_ptr);
        return true;
    }

    bool InitializePlugin(PluginModule::Plugin* plugin_ptr)
    {
        auto& plugin = *plugin_ptr;
        if (plugin.terminating || !plugin.instance) {
            return false;
        }
        if (plugin.initialized) {
            return true;
        }
        const auto context = ImGui::GetCurrentContext();
        if (!context) {
            return false;
        }
        ImGuiAllocFns fns;
        ImGui::GetAllocatorFunctions(&fns.alloc_func, &fns.free_func, &fns.user_data);
        plugin.instance->Initialize(context, fns, GWToolbox::GetDLLModule());
        plugin.instance->LoadSettings(pluginsfoldername.c_str());
        plugin.initialized = true;
        return true;
    }

    void RefreshDlls()
    {
        // when we refresh, how do we map the modules that were already loaded to the ones on disk?
        // the dll file may have changed
        namespace fs = std::filesystem;

        const fs::path plugin_folder = pluginsfoldername;

        if (!Resources::EnsureFolderExists(plugin_folder)) {
            return;
        }

        for (auto& p : fs::directory_iterator(plugin_folder)) {
            fs::path file_path = p.path();
            fs::path ext = file_path.extension();
            if (ext == ".lnk") {
                if (SUCCEEDED(Resources::ResolveShortcut(file_path, file_path))) {
                    ext = file_path.extension();
                }
            }
            if (ext == ".dll") {
                const auto found = std::ranges::find_if(plugins_available, [file_path](const auto plugin) {
                    return plugin->path == file_path;
                });
                if (found == plugins_available.end()) {
                    plugins_available.push_back(new PluginModule::Plugin(file_path));
                }
            }
        }
    }
}

void PluginModule::DrawSettingsInternal()
{
    std::scoped_lock lock(plugin_mutex);
    ImGui::PushID("Plugins");

    size_t i = 0;
    for (const auto plugin : plugins_available) {
        ImGui::PushID(i++);
        auto& style = ImGui::GetStyle();
        const auto origin_header_col = style.Colors[ImGuiCol_Header];
        style.Colors[ImGuiCol_Header] = {0, 0, 0, 0};

        static char buf[128];
        const auto has_settings = !plugin->terminating && plugin->initialized && plugin->instance && plugin->instance->HasSettings();
        if (has_settings) {
            sprintf(buf, "      %s", plugin->path.filename().string().c_str());
        }
        else {
            sprintf(buf, "             %s", plugin->path.filename().string().c_str());
        }
        const auto pos = ImGui::GetCursorScreenPos();
        const bool is_showing = has_settings ? ImGui::CollapsingHeader(buf, ImGuiTreeNodeFlags_AllowOverlap) : ImGui::CollapsingHeader(buf, ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_AllowOverlap);

        if (const auto icon = plugin->initialized && !plugin->terminating ? plugin->instance->Icon() : nullptr) {
            const float text_offset_x = ImGui::GetTextLineHeightWithSpacing() + 4.0f; // TODO: find a proper number
            ImGui::GetWindowDrawList()->AddText(
                ImVec2(pos.x + text_offset_x, pos.y + style.ItemSpacing.y / 2),
                ImColor(style.Colors[ImGuiCol_Text]), icon);
        }

        style.Colors[ImGuiCol_Header] = origin_header_col;

        ImGui::SameLine(ImGui::GetContentRegionAvail().x - ImGui::GetTextLineHeight() - ImGui::GetStyle().FramePadding.x - 128.f);
        snprintf(buf, _countof(buf), "%s###load_unload", plugin->instance ? "Unload" : "Load");
        ImGui::BeginDisabled(update_reserved || plugin->terminating);
        if (ImGui::Button(buf)) {
            if (!plugin->instance || plugin->terminating || !plugin->initialized) {
                LoadPlugin(plugin);
            }
            else {
                UnloadPlugin(plugin);
            }
        }
        ImGui::EndDisabled();
        if (plugin->version) { ImGui::SameLine(); ImGui::TextDisabled("v%u", plugin->version); }
        if (plugin->instance && !plugin->terminating && plugin->instance->GetVisiblePtr()) {
            ImGui::SameLine(ImGui::GetContentRegionAvail().x - ImGui::GetTextLineHeight() - ImGui::GetStyle().FramePadding.x);
            ImGui::Checkbox("##check", plugin->instance->GetVisiblePtr());
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Visible");
            }
        }

        if (is_showing && InitializePlugin(plugin) && has_settings) {
            plugin->instance->DrawSettings();
        }
        ImGui::PopID();
        ImGui::Separator();
    }

    if (ImGui::Button("Refresh")) {
        RefreshDlls();
    }

    ImGui::PopID();
}

bool PluginModule::CanTerminate()
{
    std::scoped_lock lock(plugin_mutex);
    return plugins_loaded.empty();
}

bool PluginModule::WndProc(const UINT msg, const WPARAM wParam, const LPARAM lParam)
{
    std::scoped_lock lock(plugin_mutex);
    bool capture = false;
    for (const auto plugin : plugins_loaded) {
        if (!plugin->instance || plugin->terminating || !plugin->initialized) {
            continue;
        }
        capture |= plugin->instance->WndProc(msg, wParam, lParam);
    }
    return capture;
}

std::vector<ToolboxPlugin*> PluginModule::GetPlugins()
{
    std::scoped_lock lock(plugin_mutex);
    std::vector<ToolboxPlugin*> plugins;
    for (const auto plugin : plugins_loaded) {
        if (!plugin->instance || plugin->terminating || !plugin->initialized) {
            continue;
        }
        plugins.push_back(plugin->instance);
        using ToolboxPluginChildInstanceFn = ToolboxPlugin* (*)(size_t);
        const auto child_instance_fn = reinterpret_cast<ToolboxPluginChildInstanceFn>(GetProcAddress(plugin->dll, "ToolboxPluginChildInstance"));
        if (!child_instance_fn) {
            continue;
        }
        constexpr auto max_child_plugins = size_t{64};
        for (auto index = size_t{0}; index < max_child_plugins; ++index) {
            const auto child = child_instance_fn(index);
            if (!child) {
                break;
            }
            if (child != plugin->instance && !std::ranges::contains(plugins, child)) {
                plugins.push_back(child);
            }
        }
    }
    return plugins;
}

void PluginModule::Initialize()
{
    std::scoped_lock lock(plugin_mutex);
    startup_settings_loaded = false;
    startup_plugins.clear();
    pluginsfoldername = Resources::GetPath(L"plugins");
    ToolboxUIElement::Initialize();
    RefreshDlls();
}

void PluginModule::Draw(IDirect3DDevice9* device)
{
    std::scoped_lock lock(plugin_mutex);
    for (const auto plugin : plugins_loaded) {
        if (!InitializePlugin(plugin)) {
            continue;
        }
        if (GW::UI::GetIsWorldMapShowing() && !plugin->instance->ShowOnWorldMap()) {
            continue;
        }

        if (const auto visibility = plugin->instance->GetVisiblePtr(); !visibility || *visibility) {
            plugin->instance->Draw(device);
        }
    }
}

void PluginModule::LoadSettings(SettingsDoc& doc, ToolboxIni* legacy)
{
    std::scoped_lock lock(plugin_mutex);
    ToolboxUIElement::LoadSettings(doc, legacy);
    std::vector<std::string> enabled_plugins;
    if (!doc.Get(Name(), "enabled_plugins", enabled_plugins) && legacy) {
        TNamesDepend dlls_to_load;
        if (legacy->GetAllKeys(plugins_enabled_section, dlls_to_load)) {
            for (const auto& entry : dlls_to_load) {
                enabled_plugins.push_back(entry.pItem);
            }
        }
    }
    if (update_reserved) return;
    const auto startup = !startup_settings_loaded;
    startup_settings_loaded = true;
    for (const auto plugin : plugins_available) plugin->enabled = false;
    std::vector<Plugin*> plugins_enabled_from_settings;
    for (const auto& entry : enabled_plugins) {
        const auto filename = std::filesystem::path(entry).filename();
        auto matching_plugins = std::views::filter(plugins_available, [filename](auto plugin) {
            return plugin->path.filename() == filename;
        });
        for (const auto plugin : matching_plugins) {
            if (startup) startup_plugins.push_back(plugin);
            plugin->enabled = true;
            if (!LoadPlugin(plugin)) {
                continue;
            }
            InitializePlugin(plugin);
            plugins_enabled_from_settings.push_back(plugin);
        }
    }
    // Find any plugins that are currently loaded but not supposed to be
    auto to_unload = std::views::filter(plugins_loaded, [&](auto plugin) {
        return !std::ranges::contains(plugins_enabled_from_settings, plugin);
    }) | std::ranges::to<std::vector>();
    for (const auto plugin : std::views::reverse(to_unload)) {
        UnloadPlugin(plugin);
    }
}

void PluginModule::SaveSettings(SettingsDoc& doc)
{
    std::scoped_lock lock(plugin_mutex);
    ToolboxUIElement::SaveSettings(doc);
    std::vector<std::string> enabled_plugins;
    for (const auto plugin : plugins_available) {
        if (plugin->initialized && plugin->instance && !plugin->terminating) plugin->instance->SaveSettings(pluginsfoldername.c_str());
        if (plugin->enabled) enabled_plugins.push_back(plugin->path.filename().string());
    }
    doc.Set(Name(), "enabled_plugins", enabled_plugins);
}

void PluginModule::Update(const float delta)
{
    std::scoped_lock lock(plugin_mutex);
    // Unloading changes plugins_loaded; iterate a snapshot instead of breaking and skipping a frame.
    for (const auto plugin : std::vector(plugins_loaded)) {
        if (plugin->terminating) {
            if (UnloadPlugin(plugin, true)) continue;
        }
        if (plugin->initialized && plugin->instance) plugin->instance->Update(delta);
    }
    static auto next_diagnostics = ULONGLONG{0};
    if (GetTickCount64() >= next_diagnostics) {
        next_diagnostics = GetTickCount64() + 1000;
        std::string diagnostics;
        for (const auto plugin : plugins_available) {
            if (!plugin->instance && !plugin->terminating) continue;
            diagnostics += std::format("{} v{} {} {}\n", plugin->path.filename().string(), plugin->version,
                plugin->sha256, plugin->terminating ? "stopping" : "loaded");
        }
        for (const auto child : GetPlugins()) diagnostics += std::format("Feature: {}\n", child->Name());
        CrashHandler::SetPluginDiagnostics(std::move(diagnostics));
    }
}

void PluginModule::SignalTerminate()
{
    std::scoped_lock lock(plugin_mutex);
    ToolboxUIElement::SignalTerminate();
    const auto snapshot = plugins_loaded;
    for (const auto plugin : snapshot) {
        UnloadPlugin(plugin, true);
    }
}

void PluginModule::Terminate()
{
    std::scoped_lock lock(plugin_mutex);
    ASSERT(plugins_loaded.empty());
    for (const auto p : plugins_available) {
        if (p->dll) {
            FreeLibrary(p->dll);
        }
        delete p;
    }
    plugins_available.clear();
    plugins_loaded.clear();
    startup_plugins.clear();
    startup_settings_loaded = false;
    ToolboxUIElement::Terminate();
}

std::vector<Gwrl::Artifact> PluginModule::Inventory(const bool refresh)
{
    std::scoped_lock lock(plugin_mutex);
    std::vector<Gwrl::Artifact> result;
    for (const auto plugin : plugins_available) {
        if (plugin->sha256.empty() || (refresh && !plugin->dll && !plugin->terminating)) plugin->sha256 = Gwrl::FileSha256(plugin->path);
        result.push_back({plugin->path.filename().string(), plugin->version, plugin->abi,
            plugin->sha256,
            TextUtils::WStringToString(plugin->path.wstring()),
            plugin->terminating ? "unloading" : plugin->initialized ? "loaded" : plugin->dll ? "loading" : "unloaded", plugin->enabled});
    }
    return result;
}

bool PluginModule::ReserveUpdate(const std::vector<std::string>& names, const bool all, std::string& error)
{
    std::scoped_lock lock(plugin_mutex);
    if (update_reserved) {
        error = "A plugin update is already pending.";
        return false;
    }
    for (const auto& name : names) {
        if (std::ranges::none_of(plugins_available, [&](auto p) { return p->path.filename() == name; })) {
            error = "Plugin is not in the discovered inventory: " + name;
            return false;
        }
    }
    for (const auto plugin : plugins_available) {
        if (!all && !std::ranges::contains(names, plugin->path.filename().string())) continue;
        if (plugin->terminating) {
            error = "A plugin is already stopping.";
            return false;
        }
        if (plugin->dll) {
            using ReadyFn = bool (*)();
            if (const auto ready = reinterpret_cast<ReadyFn>(GetProcAddress(plugin->dll, "ToolboxPluginCanPrepareUpdate")); ready && !ready()) {
                error = plugin->path.filename().string() + " has active work.";
                return false;
            }
        }
    }
    update_reserved = true;
    for (const auto plugin : plugins_available) {
        if (!all && !std::ranges::contains(names, plugin->path.filename().string())) continue;
        plugin->reserved = true;
        plugin->resume_loaded = plugin->instance != nullptr;
    }
    return true;
}

void PluginModule::StartUpdateUnload()
{
    std::scoped_lock lock(plugin_mutex);
    for (const auto plugin : std::vector(plugins_loaded)) {
        if (plugin->reserved) UnloadPlugin(plugin, true);
    }
}

bool PluginModule::UpdateReleased()
{
    std::scoped_lock lock(plugin_mutex);
    return std::ranges::none_of(plugins_available, [](auto p) { return p->reserved && (p->dll || p->instance || p->terminating); });
}

bool PluginModule::RestoreUpdate(const std::vector<Gwrl::Artifact>& expected, std::string& error)
{
    std::scoped_lock lock(plugin_mutex);
    for (const auto plugin : plugins_available) {
        if (!plugin->reserved) continue;
        const auto artifact = std::ranges::find_if(expected, [&](const auto& a) { return plugin->path.filename() == a.name; });
        if (artifact == expected.end() || plugin->terminating || plugin->dll
            || !Gwrl::IsSha256(artifact->sha256) || Gwrl::FileSha256(plugin->path) != artifact->sha256) {
            error = "Missing artifact, still loaded, or installed hash mismatch: " + plugin->path.filename().string();
            return false;
        }
    }
    auto success = true;
    for (const auto plugin : plugins_available) {
        if (!plugin->reserved) continue;
        const auto artifact = std::ranges::find_if(expected, [&](const auto& a) { return plugin->path.filename() == a.name; });
        if (!plugin->resume_loaded) {
            plugin->sha256 = artifact->sha256;
            plugin->version = artifact->version;
            plugin->abi = artifact->abi;
            continue;
        }
        if (!LoadPlugin(plugin, true) || (plugin->version && artifact->version && plugin->version != artifact->version)
            || (plugin->abi && artifact->abi && plugin->abi != artifact->abi)) {
            error = "Failed to load the expected build of " + plugin->path.filename().string();
            success = false;
            break;
        }
    }
    if (!success) StartUpdateUnload();
    return success;
}

bool PluginModule::UpdateRestored()
{
    std::scoped_lock lock(plugin_mutex);
    return std::ranges::none_of(plugins_available, [](auto p) {
        return p->reserved && p->resume_loaded && (!p->initialized || p->terminating);
    });
}

bool PluginModule::StartupComplete(std::string& error)
{
    std::scoped_lock lock(plugin_mutex);
    if (!startup_settings_loaded) return false;
    auto complete = true;
    for (const auto plugin : startup_plugins) {
        if (!plugin->instance || plugin->terminating) {
            error = "Normal startup could not initialize " + plugin->path.filename().string();
            return false;
        }
        complete = complete && plugin->initialized;
    }
    return complete;
}

void PluginModule::CancelReservation()
{
    std::scoped_lock lock(plugin_mutex);
    update_reserved = false;
    for (const auto plugin : plugins_available) {
        plugin->reserved = false;
        plugin->resume_loaded = false;
    }
}

std::unique_lock<std::recursive_mutex> PluginModule::AcquireLock()
{
    return std::unique_lock(plugin_mutex);
}
