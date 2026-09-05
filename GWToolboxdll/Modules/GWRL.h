#pragma once
#include <ToolboxModule.h>
#include <Utils/GwrlTransport.h>
#include <mutex>

class GWRL final : public ToolboxModule {
public:
    static GWRL& Instance() { static GWRL instance; return instance; }
    const char* Name() const override { return "GWRL"; }
    const char* Icon() const override { return ICON_FA_SYNC; }
    void Initialize() override;
    void SignalTerminate() override;
    bool CanTerminate() override;
    void Update(float) override;
    void Draw(IDirect3DDevice9*) override;
    void DrawSettingsInternal() override;

private:
    void Handle(const Gwrl::Message& request);
    void Send(Gwrl::Message message);
    void Reply(const Gwrl::Message& request, const char* type, const std::string& code = {}, const std::string& detail = {});
    std::vector<Gwrl::Artifact> Inventory(bool refresh = false) const;
    void DrawStatus();
    mutable std::recursive_mutex mutex_;
    Gwrl::Transport transport_;
    Gwrl::Artifact toolbox_;
    std::vector<Gwrl::Artifact> available_, original_, expected_;
    std::string session_, transaction_, state_ = "idle", detail_;
    std::string last_request_, last_response_;
    std::unordered_map<std::string, std::pair<std::string, std::string>> replies_;
    uint64_t generation_ = 0, last_received_ = 0, next_ping_ = 0, request_number_ = 0;
    bool welcomed_ = false, stopping_ = false, restarted_ = false, full_update_ = false, show_notification_ = false;
    bool startup_reported_ = false, shutdown_notified_ = false, shutdown_started_ = false;
};
