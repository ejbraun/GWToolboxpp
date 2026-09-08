#pragma once
#include <GWRL/Api.h>
#include <GWRL/Transport.h>
#include <memory>

namespace Gwrl {
    enum class Routed { Invalid, Update, Handled };
    struct RoutingDiagnostic {
        uint64_t count = 0;
        std::string code, recipient, type, request_id;
    };

    class Router {
    public:
        explicit Router(Transport& transport, std::string client = "toolbox");
        ~Router();
        Router(const Router&) = delete;
        Router& operator=(const Router&) = delete;
        const GwrlApi* Api() const;
        void BeginSession(const std::string& session, uint64_t generation);
        glz::raw_json Offer();
        glz::raw_json Welcome(bool negotiated, const std::optional<glz::raw_json>& selection);
        void FinishWelcome(bool queued);
        void Suspend();
        bool Negotiated() const;
        RoutingDiagnostic Diagnostic() const;
        Routed Route(const ReceivedFrame& frame, Envelope& envelope);
        void Pump();
        void Quiesce();
        void OpenOwner(uintptr_t owner);
        void CloseOwner(uintptr_t owner);
        bool OwnerDrained(uintptr_t owner) const;
        void Close();
        bool Drained() const;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
}
