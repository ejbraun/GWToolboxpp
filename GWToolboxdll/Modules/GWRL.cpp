#include "stdafx.h"
#include "GWRL.h"
#include <ForkVersion.h>
#include <GWToolbox.h>
#include <Modules/CrashHandler.h>
#include <Modules/PluginModule.h>
#include <Utils/TextUtils.h>

template<> struct glz::meta<Gwrl::Message> {
    template<class T> static bool skip_if(const T& value, const std::string_view key, const glz::meta_context&)
    {
        if constexpr (std::is_same_v<T, std::vector<Gwrl::Artifact>>) return key == "artifacts" && value.empty();
        return false;
    }
};

namespace {
    constexpr auto json_options = glz::opts{.error_on_unknown_keys = false};
}

void GWRL::Initialize()
{
    ToolboxModule::Initialize();
    wchar_t path[32768]{};
    const auto count = GetModuleFileNameW(GWToolbox::GetDLLModule(), path, _countof(path));
    const auto module_path = count && count < _countof(path) ? std::wstring(path, count) : std::wstring();
    toolbox_ = {"GWToolboxdll.dll", GWTOOLBOX_FORK_VERSION, GWTOOLBOX_PLUGIN_ABI,
        module_path.empty() ? "" : Gwrl::FileSha256(module_path), TextUtils::WStringToString(module_path), "loaded", true};
    if (const auto descriptor = Gwrl::ReadBootstrap(); descriptor && descriptor->transaction_id[0]) {
        transaction_ = descriptor->transaction_id;
        restarted_ = full_update_ = true;
        state_ = "starting";
        detail_ = "Toolbox is restoring its configuration through normal startup.";
    }
    transport_.Start();
}

void GWRL::SignalTerminate()
{
    std::scoped_lock lock(mutex_);
    stopping_ = true;
    welcomed_ = false;
    transport_.Stop();
}

bool GWRL::CanTerminate()
{
    return transport_.Stopped();
}

std::vector<Gwrl::Artifact> GWRL::Inventory(const bool refresh) const
{
    auto inventory = PluginModule::Inventory(refresh);
    std::erase_if(inventory, [](const auto& artifact) { return !Gwrl::IsManagedPlugin(artifact.name); });
    inventory.insert(inventory.begin(), toolbox_);
    return inventory;
}

bool GWRL::Send(Gwrl::Message message)
{
    message.session_id = session_;
    message.pid = GetCurrentProcessId();
    message.process_started = std::to_string(Gwrl::ProcessStarted(GetCurrentProcess()));
    message.state = state_;
    if (message.transaction_id.empty() && message.type != "update_request") message.transaction_id = transaction_;
    std::string json;
    if (glz::write_json(message, json) || !transport_.Send(json)) {
        welcomed_ = false;
        detail_ = "GWRL connection is unavailable. The launcher must reconnect to continue.";
        return false;
    }
    last_response_ = std::move(json);
    return true;
}

void GWRL::Reply(const Gwrl::Message& request, const char* type, const std::string& code, const std::string& detail)
{
    Gwrl::Message message;
    message.type = type;
    message.request_id = request.request_id;
    message.transaction_id = message.type == "error" ? request.transaction_id : transaction_;
    message.code = code.empty() && state_ == "failed" ? "startup_failed" : code;
    message.detail = detail.empty() ? detail_ : detail;
    if (message.type != "error") message.artifacts = Inventory();
    Send(std::move(message));
}

void GWRL::Handle(const Gwrl::Message& request)
{
    if (request.type == "welcome") {
        if (!std::ranges::contains(request.capabilities, "cooperative_update_v1")
            || !std::ranges::contains(request.capabilities, "normal_lifecycle_v1")) {
            welcomed_ = false;
            Reply(request, "error", "unsupported_capability", "cooperative_update_v1 and normal_lifecycle_v1 are required.");
            return;
        }
        startup_reported_ = false;
        welcomed_ = true;
        Reply(request, "status");
        return;
    }
    if (!welcomed_) { Reply(request, "error", "handshake_required"); return; }
    if (request.type == "ping") { Reply(request, "pong"); return; }
    if (request.type == "ack" || request.type == "error") {
        if (!pending_request_.empty() && request.request_id == pending_request_) {
            if (request.type == "ack") {
                request_acknowledged_ = true;
                detail_ = "GWRL acknowledged the request. Waiting for the launcher to stage files and prepare the update.";
            }
            else {
                detail_ = std::format("GWRL rejected the update request: {} {}", request.code, request.detail);
                pending_request_.clear();
            }
        }
        else if (request.type == "error") {
            detail_ = std::format("GWRL reported: {} {}", request.code, request.detail);
        }
        return;
    }
    if (request.type == "pong") return;
    if (request.type == "get_inventory" || request.type == "query_transaction") { Reply(request, "status"); return; }
    if (request.type == "updates_available") {
        if (!request.artifacts.empty() && !Gwrl::ValidatePlan(request.artifacts)) {
            Reply(request, "error", "invalid_plan"); return;
        }
        for (const auto& artifact : request.artifacts) {
            const auto identity = std::format("{}:{}:{}:{}", artifact.name, artifact.version, artifact.abi, artifact.sha256);
            if (!std::ranges::contains(announced_versions_, identity)) {
                announced_versions_.push_back(identity);
                show_notification_ = true;
            }
        }
        available_ = request.artifacts;
        Reply(request, "ack");
        return;
    }
    if (request.type == "prepare_update") {
        if (state_ != "idle" || !transaction_.empty()) { Reply(request, "error", "transaction_busy"); return; }
        if (!request.user_initiated || !Gwrl::IsIdentifier(request.transaction_id) || !Gwrl::ValidatePlan(request.artifacts)) {
            Reply(request, "error", "explicit_request_required", "A user-approved plan and transaction identifier are required."); return;
        }
        const auto inventory = Inventory(true);
        auto all = false;
        std::vector<std::string> names;
        for (const auto& artifact : request.artifacts) {
            const auto found = std::ranges::find_if(inventory, [&](const auto& a) { return a.name == artifact.name; });
            if (found == inventory.end() || std::ranges::count_if(inventory, [&](const auto& a) { return a.name == artifact.name; }) != 1 || !Gwrl::IsSha256(found->sha256)
                || ((artifact.name == "DBBox.dll" || artifact.name == "GWToolboxdll.dll") && artifact.abi != GWTOOLBOX_PLUGIN_ABI)) {
                Reply(request, "error", "unsupported_artifact", artifact.name); return;
            }
            if (artifact.name == "GWToolboxdll.dll") all = true;
            else names.push_back(artifact.name);
        }
        std::string error;
        if (!PluginModule::ReserveUpdate(names, all, error)) { Reply(request, "error", "plugin_busy", error); return; }
        original_.clear();
        for (const auto& a : inventory) {
            if (std::ranges::any_of(request.artifacts, [&](const auto& target) { return target.name == a.name; })) original_.push_back(a);
        }
        expected_ = original_;
        for (const auto& artifact : request.artifacts) {
            auto& target = *std::ranges::find_if(expected_, [&](const auto& a) { return a.name == artifact.name; });
            target.version = artifact.version;
            target.abi = artifact.abi;
            target.sha256 = artifact.sha256;
        }
        pending_request_.clear();
        transaction_ = request.transaction_id;
        state_ = "prepared";
        full_update_ = all;
        detail_ = "Prepared. Waiting for the launcher to release all affected clients.";
        Reply(request, "prepared");
        return;
    }
    if (request.transaction_id != transaction_ || transaction_.empty()) { Reply(request, "error", "wrong_transaction"); return; }
    if (request.type == "begin_unload") {
        if (state_ != "prepared") { Reply(request, "error", "invalid_state"); return; }
        if (full_update_) {
            state_ = "shutting_down";
            detail_ = "Toolbox is exiting through its normal save and shutdown path.";
            shutdown_notified_ = false;
            Reply(request, "accepted");
        }
        else {
            state_ = "unloading";
            detail_ = "Waiting for selected plugins to save settings and release their DLLs.";
            PluginModule::StartUpdateUnload();
            Reply(request, "accepted");
        }
        return;
    }
    if (request.type == "finish_update") {
        if (state_ != "ready" && state_ != "idle") { Reply(request, "error", "invalid_state"); return; }
        PluginModule::CancelReservation();
        state_ = "idle";
        detail_ = "Update transaction complete.";
        show_notification_ = false;
        announced_versions_.clear();
        available_.clear();
        Reply(request, "ack");
        transaction_.clear();
        original_.clear();
        expected_.clear();
        full_update_ = restarted_ = false;
        return;
    }
    if (request.type == "rollback_update") {
        if (restarted_ && (state_ == "starting" || state_ == "ready" || state_ == "failed")) {
            std::string error;
            if (!PluginModule::ReserveUpdate({}, true, error)) { Reply(request, "error", "plugin_busy", error); return; }
            state_ = "shutting_down";
            detail_ = "Toolbox is exiting normally so GWRL can restore the original files.";
            shutdown_notified_ = false;
            Reply(request, "accepted");
            return;
        }
        if (full_update_ || state_ != "ready") { Reply(request, "error", "invalid_state"); return; }
        state_ = "unloading";
        detail_ = "Releasing restored components so GWRL can roll back the transaction.";
        PluginModule::StartUpdateUnload();
        Reply(request, "accepted");
        return;
    }
    if (request.type == "commit_update" || request.type == "abort_update") {
        const auto abort = request.type == "abort_update";
        if (abort && state_ == "prepared") {
            PluginModule::CancelReservation();
            state_ = "idle";
            detail_ = "Update cancelled before unloading.";
            Reply(request, "ready");
            return;
        }
        if (full_update_ || state_ != "released") { Reply(request, "error", "invalid_state"); return; }
        const auto& restore = abort ? original_ : expected_;
        std::string error;
        if (!PluginModule::RestoreUpdate(restore, error)) {
            state_ = PluginModule::UpdateReleased() ? "released" : "unloading";
            detail_ = error;
            Reply(request, "error", "restore_failed", error);
            return;
        }
        state_ = "reloading";
        detail_ = "Initializing restored plugins.";
        Reply(request, "accepted");
        return;
    }
    Reply(request, "error", "unknown_message");
}

void GWRL::Update(float)
{
    std::scoped_lock lock(mutex_);
    if (stopping_) return;
    const auto now = GetTickCount64();
    const auto connected = transport_.Connected();
    if (state_ == "starting" && GWToolbox::IsInitialized()) {
        std::string error;
        if (!GWToolbox::IsModuleEnabled(&PluginModule::Instance()) || PluginModule::StartupComplete(error)) {
            state_ = "ready";
            available_.clear();
            detail_ = "Normal Toolbox startup completed. Waiting for GWRL to finish all affected clients.";
        }
        else if (!error.empty()) {
            state_ = "failed";
            detail_ = error;
        }
    }
    if (!connected) {
        welcomed_ = false;
        if (!pending_request_.empty()) {
            detail_ = request_acknowledged_
                ? "GWRL disconnected after acknowledging the request. Check the launcher for progress before retrying."
                : "GWRL disconnected before acknowledging the request. No update was started by Toolbox; check the launcher.";
            pending_request_.clear();
        }
    }
    if (!pending_request_.empty() && now - request_sent_ > (request_acknowledged_ ? 300000u : 30000u)) {
        detail_ = request_acknowledged_
            ? "GWRL acknowledged the request but has not prepared an update. Check the launcher for download progress or errors."
            : "GWRL did not acknowledge the update request. Check the launcher before retrying.";
        pending_request_.clear();
    }
    if (connected && generation_ != transport_.Generation()) {
        generation_ = transport_.Generation();
        const auto descriptor = transport_.Connection();
        if (!descriptor) return;
        if (session_ != descriptor->session_id) replies_.clear();
        session_ = descriptor->session_id;
        welcomed_ = false;
        startup_reported_ = shutdown_notified_ = false;
        last_received_ = now;
        next_ping_ = now + 5000;
        Gwrl::Message hello;
        hello.type = "hello";
        hello.detail = std::format("Toolbox {} ({})", GWTOOLBOX_FORK_DISPLAY_VERSION, GWTOOLBOX_FORK_BUILD_ID);
        hello.capabilities = {"cooperative_update_v1", "normal_lifecycle_v1", "plugin_reload", "toolbox_unload"};
        if (state_ == "failed") hello.code = "startup_failed";
        hello.artifacts = Inventory();
        Send(std::move(hello));
    }
    for (const auto& json : transport_.Receive()) {
        if (!transport_.Connected()) break;
        Gwrl::Message request;
        if (json.size() > Gwrl::MaximumPayload || glz::read<json_options>(request, json)
            || !Gwrl::ValidateEnvelope(request, session_, GetCurrentProcessId(), Gwrl::ProcessStarted(GetCurrentProcess()))) continue;
        last_received_ = now;
        const auto mutation = request.type != "ping" && request.type != "pong" && request.type != "get_inventory"
            && request.type != "query_transaction" && request.type != "welcome" && request.type != "updates_available"
            && request.type != "ack" && request.type != "error";
        if (mutation) {
            if (const auto prior = replies_.find(request.request_id); prior != replies_.end()) {
                if (prior->second.first != json) Reply(request, "error", "request_id_reused");
                else if (!transport_.Send(prior->second.second)) welcomed_ = false;
                continue;
            }
            if (replies_.size() >= 128) { Reply(request, "error", "reconnect_required"); continue; }
        }
        last_response_.clear();
        Handle(request);
        if (mutation) replies_.emplace(request.request_id, std::pair{json, last_response_});
    }
    if (welcomed_ && now - last_received_ > 15000) {
        welcomed_ = false;
        detail_ = "GWRL stopped responding. Updates are paused until the launcher reconnects.";
    }
    if (connected && now >= next_ping_) {
        next_ping_ = now + 5000;
        Gwrl::Message ping;
        ping.type = "ping";
        ping.request_id = "tb-" + std::to_string(++request_number_);
        Send(std::move(ping));
    }
    if (state_ == "unloading" && PluginModule::UpdateReleased()) {
        state_ = "released";
        detail_ = "Selected plugins released. Waiting for GWRL to install or restore their files.";
        Gwrl::Message event;
        event.type = state_;
        event.artifacts = Inventory();
        Send(std::move(event));
    }
    if (restarted_ && welcomed_ && !startup_reported_ && (state_ == "ready" || state_ == "failed")) {
        Gwrl::Message event;
        event.type = state_ == "failed" ? "error" : "ready";
        if (state_ == "failed") event.code = "startup_failed";
        event.detail = detail_;
        event.artifacts = Inventory();
        Send(std::move(event));
        startup_reported_ = welcomed_;
    }
    if (state_ == "shutting_down" && welcomed_ && connected && !shutdown_notified_) {
        Gwrl::Message event;
        event.type = "shutdown_starting";
        event.detail = detail_;
        Send(std::move(event));
        shutdown_notified_ = welcomed_;
    }
    if (state_ == "shutting_down" && shutdown_notified_ && !shutdown_started_
        && welcomed_ && connected && transport_.Flushed()) {
        shutdown_started_ = true;
        GWToolbox::SignalTerminate();
    }
    if (state_ == "reloading" && PluginModule::UpdateRestored()) {
        state_ = "ready";
        available_.clear();
        detail_ = "Plugins restored. Waiting for GWRL to finish all affected clients.";
        Gwrl::Message event;
        event.type = "ready";
        event.artifacts = Inventory();
        Send(std::move(event));
    }
    CrashHandler::SetUpdateDiagnostics(std::format("GWRL transaction={} state={} connected={}\n", transaction_, state_, welcomed_));
}

void GWRL::DrawStatus()
{
    ImGui::TextUnformatted(welcomed_ && transport_.Connected() ? "Connected to GWRLauncher" : "GWRLauncher is not connected");
    if (!detail_.empty()) ImGui::TextWrapped("%s", detail_.c_str());
    if (available_.empty()) ImGui::TextDisabled("No pending updates reported by GWRL.");
    for (const auto& artifact : available_) ImGui::BulletText("%s  v%u", artifact.name.c_str(), artifact.version);
    ImGui::BeginDisabled(!welcomed_ || !transport_.Connected() || available_.empty() || state_ != "idle" || !transaction_.empty() || !pending_request_.empty());
    if (ImGui::Button("Update now")) {
        Gwrl::Message request;
        request.type = "update_request";
        request.request_id = "tb-" + std::to_string(++request_number_);
        request.user_initiated = true;
        request.artifacts = available_;
        const auto request_id = request.request_id;
        if (Send(std::move(request))) {
            pending_request_ = request_id;
            request_sent_ = GetTickCount64();
            request_acknowledged_ = false;
            detail_ = "Update request sent. Waiting for GWRL to acknowledge it.";
        }
    }
    ImGui::EndDisabled();
}

void GWRL::DrawSettingsInternal()
{
    std::scoped_lock lock(mutex_);
    ImGui::TextWrapped("GWRLauncher manages downloads and updates. Updates only start when you request them here or in the launcher.");
    DrawStatus();
}

void GWRL::Draw(IDirect3DDevice9*)
{
    std::scoped_lock lock(mutex_);
    if (!show_notification_ || stopping_) return;
    ImGui::SetNextWindowSize(ImVec2(360.f, 0.f), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("GWRL updates", &show_notification_, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoFocusOnAppearing)) DrawStatus();
    ImGui::End();
}
