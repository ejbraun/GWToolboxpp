#include <GWRL/Router.h>
#include <algorithm>
#include <chrono>
#include <deque>
#include <map>
#include <set>
#include <utility>

namespace Gwrl {
    namespace {
        constexpr auto RequestTimeout = uint64_t{30000};
        constexpr auto RefreshTimeout = uint64_t{5000};
        constexpr auto MaximumTokens = size_t{4096};
        constexpr auto MaximumDispatch = size_t{MaximumRoutes * MaximumRouteQueue * 2};

        GwrlBytes Bytes(const std::string& value) { return {value.data(), static_cast<uint32_t>(value.size())}; }
        bool ValidBytes(const GwrlBytes value, const uint32_t maximum) { return value.size <= maximum && (!value.size || value.data); }
        std::string Copy(const GwrlBytes value) { return value.size ? std::string(value.data, value.size) : std::string(); }
        template<class T> glz::raw_json Json(const T& value)
        {
            glz::raw_json result;
            if (glz::write_json(value, result.str)) throw std::runtime_error("GWRL JSON serialization failed");
            return result;
        }
    }

    struct Router::Impl {
        struct Dispatch {
            Envelope message;
            uint64_t request = 0;
        };
        struct Registration {
            uint64_t handle = 0;
            RouteDescriptor descriptor;
            GwrlRouteConfig callbacks{};
            std::optional<RouteSelection> selection;
            std::deque<Dispatch> pending;
            uint32_t active = 0;
            bool closing = false, notify = true;
        };
        struct Pending {
            uint64_t registration = 0;
            Envelope message;
            uint64_t deadline = 0;
            bool incoming = false;
        };

        Transport& transport;
        std::string client, session;
        uint32_t pid = GetCurrentProcessId();
        uint64_t started = ProcessStarted(GetCurrentProcess());
        mutable std::mutex mutex;
        std::map<uint64_t, std::shared_ptr<Registration>> registrations;
        std::map<uint64_t, Pending> requests;
        std::set<std::string> used_tokens;
        std::set<uintptr_t> closing_owners;
        uint64_t generation = 0, serial = 0, revision = 1, offered_revision = 0, cursor = 0;
        uint64_t refresh_deadline = 0;
        uint32_t refresh_attempts = 0;
        std::string offer_id, refresh_json, selection_json, ack_json;
        RouteAck welcome_ack;
        bool online = false, handshake = false, enabled = false, routing_envelope = false, quiescing = false, closing = false, dirty = false;
        GwrlApi api{};
        RoutingDiagnostic diagnostic;

        explicit Impl(Transport& value, std::string adapter) : transport(value), client(std::move(adapter)) { }

        Envelope Message(const std::string& type) const
        {
            Envelope result;
            result.client = client;
            result.session_id = session;
            result.pid = pid;
            result.process_started = std::to_string(started);
            result.type = type;
            result.recipient = "gwrl";
            return result;
        }

        RouteOffer OfferLocked()
        {
            RouteOffer result;
            offered_revision = revision;
            result.revision = std::to_string(revision);
            for (const auto& [id, route] : registrations) {
                if (!route->closing) result.routes.push_back(route->descriptor);
            }
            return result;
        }

        size_t Queued() const
        {
            auto count = size_t{0};
            for (const auto& [id, route] : registrations) count += route->pending.size();
            return count;
        }

        void Record(const std::string& code, const Envelope& message)
        {
            ++diagnostic.count;
            diagnostic.code = code;
            diagnostic.recipient = message.recipient.value_or("gwrl").substr(0, 64);
            diagnostic.type = message.type.substr(0, 64);
            diagnostic.request_id = message.request_id.substr(0, 64);
        }

        bool Control(const std::string& json)
        {
            if (transport.Send(json, generation)) return true;
            Record("control_not_queued", Message("routing"));
            return false;
        }

        void Error(const Envelope& request, const std::string& code)
        {
            if (request.kind != "request") return;
            Record(code, request);
            auto reply = Message("error");
            reply.recipient = request.recipient;
            reply.kind = "response";
            reply.request_id = request.request_id;
            reply.route_version = request.route_version;
            reply.route_session = request.route_session;
            reply.payload = glz::raw_json{"{}"};
            reply.code = code;
            reply.detail = code;
            const auto json = Json(reply).str;
            if (transport.SendModule(json, generation, UINT64_MAX) != Delivery::Queued) Record("error_not_queued", request);
        }

        void Invalidate(Registration& route, const bool report)
        {
            transport.CancelModule(route.handle);
            if (report) std::erase_if(route.pending, [](const auto& work) { return work.message.kind != "response"; });
            else route.pending.clear();
            for (auto it = requests.begin(); it != requests.end();) {
                if (it->second.registration != route.handle) { ++it; continue; }
                if (it->second.incoming && handshake && enabled && !quiescing) Error(it->second.message, "recipient_unavailable");
                if (report && !it->second.incoming) {
                    auto response = it->second.message;
                    response.kind = "response";
                    response.type = "error";
                    response.payload = glz::raw_json{"{}"};
                    response.code = "route_unavailable";
                    response.detail = "The route changed; the result of an earlier request may be unknown.";
                    route.pending.push_back({std::move(response), it->first});
                }
                it = requests.erase(it);
            }
            route.selection.reset();
            route.notify = true;
        }

        void Remove(Registration& route)
        {
            if (route.closing) return;
            route.closing = true;
            Invalidate(route, false);
            route.notify = false;
            ++revision;
            dirty = true;
        }

        void Sweep()
        {
            std::erase_if(registrations, [](const auto& item) { return item.second->closing && !item.second->active; });
        }

        void SuspendLocked()
        {
            enabled = false;
            online = handshake = false;
            transport.CancelModule();
            for (const auto& [id, route] : registrations) if (!route->closing) Invalidate(*route, true);
            offer_id.clear();
            refresh_json.clear();
            selection_json.clear();
            ack_json.clear();
            welcome_ack = {};
        }

        RouteAck Select(const std::optional<glz::raw_json>& raw)
        {
            RouteAck ack;
            ack.revision = std::to_string(offered_revision);
            RouteSelections selection;
            if (!raw || !JsonObject(raw->str) || glz::read<EnvelopeOptions>(selection, raw->str)
                || selection.routes.size() > MaximumRoutes) { ack.code = "invalid_route_selection"; return ack; }
            ack.revision = selection.revision;
            if (selection.revision != std::to_string(revision) || offered_revision != revision) {
                ack.code = "stale_route_list"; return ack;
            }
            std::set<std::string> recipients, tokens;
            for (const auto& selected : selection.routes) {
                const auto found = std::ranges::find_if(registrations, [&](const auto& item) {
                    return !item.second->closing && item.second->descriptor.recipient == selected.recipient;
                });
                if (found == registrations.end() || !recipients.insert(selected.recipient).second
                    || found->second->descriptor.registration_id != selected.registration_id
                    || !IsToken(selected.peer_registration_id) || !IsToken(selected.route_session)
                    || !tokens.insert(selected.route_session).second) { ack.code = "invalid_route_selection"; return ack; }
                const auto& route = *found->second;
                if (!std::ranges::contains(route.descriptor.versions, selected.route_version)) {
                    ack.code = "unsupported_route_version"; return ack;
                }
                if (route.selection == selected) continue;
                if (route.selection && route.selection->peer_registration_id == selected.peer_registration_id
                    && route.selection->route_version == selected.route_version) { ack.code = "route_session_changed"; return ack; }
                if (used_tokens.contains(selected.route_session)) { ack.code = "stale_route_session"; return ack; }
            }
            if (used_tokens.size() + selection.routes.size() > MaximumTokens) { ack.code = "routing_session_exhausted"; return ack; }
            ack.accepted = true;
            ack.routes = std::move(selection.routes);
            return ack;
        }

        void Apply(const RouteAck& ack)
        {
            if (!ack.accepted || ack.revision != std::to_string(revision) || !enabled || quiescing) return;
            for (const auto& [id, route] : registrations) {
                if (route->closing) continue;
                const auto found = std::ranges::find(ack.routes, route->descriptor.recipient, &RouteSelection::recipient);
                if (found == ack.routes.end()) {
                    if (route->selection) Invalidate(*route, true);
                    continue;
                }
                if (route->selection == *found) continue;
                Invalidate(*route, true);
                used_tokens.insert(found->route_session);
                route->selection = *found;
                route->notify = true;
            }
        }

        GwrlResult Register(const GwrlRouteConfig* config, GwrlRegistration* result)
        {
            if (!config || config->size < sizeof(GwrlRouteConfig) || !result || !config->on_message
                || !config->owner_module || !ValidBytes(config->recipient, 64) || !config->versions
                || !config->version_count || config->version_count > MaximumVersions) return GWRL_INVALID_ARGUMENT;
            const auto recipient = Copy(config->recipient);
            if (!IsRecipient(recipient) || recipient == "gwrl") return GWRL_INVALID_ARGUMENT;
            auto versions = std::vector<uint32_t>(config->versions, config->versions + config->version_count);
            std::ranges::sort(versions);
            if (!versions.front() || std::adjacent_find(versions.begin(), versions.end()) != versions.end()) return GWRL_INVALID_ARGUMENT;
            HMODULE owner = nullptr;
            if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCWSTR>(config->on_message), &owner) || reinterpret_cast<uintptr_t>(owner) != config->owner_module) return GWRL_INVALID_ARGUMENT;
            if (config->on_availability && (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCWSTR>(config->on_availability), &owner) || reinterpret_cast<uintptr_t>(owner) != config->owner_module)) return GWRL_INVALID_ARGUMENT;
            std::scoped_lock lock(mutex);
            if (closing || quiescing || closing_owners.contains(config->owner_module)) return GWRL_CLOSING;
            Sweep();
            if (registrations.size() >= MaximumRoutes) return GWRL_QUEUE_FULL;
            for (const auto& [id, route] : registrations) if (route->descriptor.recipient == recipient) return GWRL_DUPLICATE_ROUTE;
            auto route = std::make_shared<Registration>();
            route->handle = ++serial;
            route->descriptor = {recipient, NewToken(), std::move(versions)};
            route->callbacks = *config;
            route->callbacks.recipient = {};
            route->callbacks.versions = nullptr;
            registrations.emplace(route->handle, route);
            *result = route->handle;
            ++revision;
            dirty = true;
            return GWRL_QUEUED;
        }

        GwrlResult Send(const uint64_t registration, const GwrlBytes type, const GwrlBytes payload, GwrlRequest* result)
        {
            if (payload.size > MaximumPayload) return GWRL_OVERSIZED;
            if (!ValidBytes(type, 64) || !ValidBytes(payload, MaximumPayload) || !IsIdentifier(Copy(type))) return GWRL_INVALID_ARGUMENT;
            const auto body = Copy(payload);
            if (!JsonObject(body)) return payload.size > MaximumPayload ? GWRL_OVERSIZED : GWRL_INVALID_ARGUMENT;
            std::scoped_lock lock(mutex);
            const auto found = registrations.find(registration);
            if (found == registrations.end() || found->second->closing || closing || quiescing) return GWRL_CLOSING;
            if (!online || !transport.Connected() || transport.Generation() != generation) return GWRL_DISCONNECTED;
            const auto& route = *found->second;
            if (!enabled || !route.selection) return GWRL_NOT_NEGOTIATED;
            if (result && std::cmp_greater_equal(std::ranges::count_if(requests, [&](const auto& item) {
                return item.second.registration == registration && !item.second.incoming;
            }) + std::ranges::count_if(route.pending, [](const auto& work) { return work.message.kind == "response"; }), MaximumRouteQueue)) return GWRL_QUEUE_FULL;
            auto message = Message(Copy(type));
            const auto request = result ? ++serial : 0;
            message.recipient = route.descriptor.recipient;
            message.route_version = route.selection->route_version;
            message.route_session = route.selection->route_session;
            message.kind = result ? "request" : "event";
            message.request_id = result ? "m-" + std::to_string(request) : "";
            message.payload = glz::raw_json{body};
            const auto delivery = transport.SendModule(Json(message).str, generation, registration);
            if (delivery == Delivery::Disconnected) return GWRL_DISCONNECTED;
            if (delivery == Delivery::Oversized) return GWRL_OVERSIZED;
            if (delivery == Delivery::QueueFull) return GWRL_QUEUE_FULL;
            if (result) {
                requests.emplace(request, Pending{registration, std::move(message), GetTickCount64() + RequestTimeout, false});
                *result = request;
            }
            return GWRL_QUEUED;
        }

        GwrlResult Reply(const uint64_t context, const GwrlBytes type, const GwrlBytes payload, const GwrlBytes code, const GwrlBytes detail)
        {
            if (payload.size > MaximumPayload) return GWRL_OVERSIZED;
            if (!ValidBytes(type, 64) || !ValidBytes(payload, MaximumPayload) || !ValidBytes(code, 64)
                || !ValidBytes(detail, 1024) || !IsIdentifier(Copy(type)) || !JsonObject(Copy(payload))) return GWRL_INVALID_ARGUMENT;
            std::scoped_lock lock(mutex);
            const auto pending = requests.find(context);
            if (pending == requests.end() || !pending->second.incoming || GetTickCount64() >= pending->second.deadline) return GWRL_EXPIRED;
            const auto found = registrations.find(pending->second.registration);
            if (found == registrations.end() || found->second->closing || !enabled || quiescing) return GWRL_EXPIRED;
            auto response = Message(Copy(type));
            response.recipient = pending->second.message.recipient;
            response.request_id = pending->second.message.request_id;
            response.route_version = pending->second.message.route_version;
            response.route_session = pending->second.message.route_session;
            response.kind = "response";
            response.payload = glz::raw_json{Copy(payload)};
            if (code.size) response.code = Copy(code);
            if (detail.size) response.detail = Copy(detail);
            const auto delivery = transport.SendModule(Json(response).str, generation, found->first);
            if (delivery == Delivery::Disconnected) return GWRL_DISCONNECTED;
            if (delivery == Delivery::Oversized) return GWRL_OVERSIZED;
            if (delivery == Delivery::QueueFull) return GWRL_QUEUE_FULL;
            requests.erase(pending);
            return GWRL_QUEUED;
        }
    };

    Router::Router(Transport& transport, std::string client) : impl_(std::make_unique<Impl>(transport, std::move(client)))
    {
        auto& api = impl_->api;
        api.size = sizeof(api);
        api.abi = 1;
        api.context = impl_.get();
        api.register_route = [](void* context, const GwrlRouteConfig* config, GwrlRegistration* result) -> GwrlResult {
            try { return static_cast<Impl*>(context)->Register(config, result); } catch (...) { return GWRL_INTERNAL_ERROR; }
        };
        api.send_request = [](void* context, GwrlRegistration route, GwrlBytes type, GwrlBytes payload, GwrlRequest* result) -> GwrlResult {
            if (!result) return GWRL_INVALID_ARGUMENT;
            try { return static_cast<Impl*>(context)->Send(route, type, payload, result); } catch (...) { return GWRL_INTERNAL_ERROR; }
        };
        api.emit_event = [](void* context, GwrlRegistration route, GwrlBytes type, GwrlBytes payload) -> GwrlResult {
            try { return static_cast<Impl*>(context)->Send(route, type, payload, nullptr); } catch (...) { return GWRL_INTERNAL_ERROR; }
        };
        api.reply = [](void* context, GwrlRequest request, GwrlBytes type, GwrlBytes payload, GwrlBytes code, GwrlBytes detail) -> GwrlResult {
            try { return static_cast<Impl*>(context)->Reply(request, type, payload, code, detail); } catch (...) { return GWRL_INTERNAL_ERROR; }
        };
        api.release_request = [](void* context, GwrlRequest request) -> GwrlResult {
            try {
                auto& self = *static_cast<Impl*>(context);
                std::scoped_lock lock(self.mutex);
                const auto found = self.requests.find(request);
                if (found == self.requests.end() || !found->second.incoming) return GWRL_EXPIRED;
                self.Error(found->second.message, "request_cancelled");
                const auto route = self.registrations.find(found->second.registration);
                if (route != self.registrations.end()) std::erase_if(route->second->pending, [&](const auto& work) { return work.request == request; });
                self.requests.erase(found);
                return GWRL_QUEUED;
            } catch (...) { return GWRL_INTERNAL_ERROR; }
        };
        api.begin_unregister = [](void* context, GwrlRegistration registration) -> GwrlResult {
            try {
                auto& self = *static_cast<Impl*>(context);
                std::scoped_lock lock(self.mutex);
                const auto found = self.registrations.find(registration);
                if (found == self.registrations.end()) return GWRL_EXPIRED;
                self.Remove(*found->second);
                self.Sweep();
                return GWRL_QUEUED;
            } catch (...) { return GWRL_INTERNAL_ERROR; }
        };
        api.is_drained = [](void* context, GwrlRegistration registration) -> uint32_t {
            try {
                auto& self = *static_cast<Impl*>(context);
                std::scoped_lock lock(self.mutex);
                return self.registrations.find(registration) == self.registrations.end();
            } catch (...) { return 0; }
        };
    }

    Router::~Router() = default;
    const GwrlApi* Router::Api() const { return &impl_->api; }

    void Router::BeginSession(const std::string& session, const uint64_t generation)
    {
        auto& self = *impl_;
        std::scoped_lock lock(self.mutex);
        self.SuspendLocked();
        if (self.session != session) self.used_tokens.clear();
        self.session = session;
        self.generation = generation;
        self.online = true;
        self.offered_revision = 0;
        self.routing_envelope = false;
        self.dirty = false;
    }

    glz::raw_json Router::Offer()
    {
        auto& self = *impl_;
        std::scoped_lock lock(self.mutex);
        return Json(self.OfferLocked());
    }

    glz::raw_json Router::Welcome(const bool negotiated, const std::optional<glz::raw_json>& selection)
    {
        auto& self = *impl_;
        std::scoped_lock lock(self.mutex);
        self.SuspendLocked();
        self.online = self.handshake = true;
        self.routing_envelope = negotiated;
        self.enabled = negotiated && !self.closing && !self.quiescing;
        if (!self.enabled) {
            self.welcome_ack.revision = std::to_string(self.revision);
            self.welcome_ack.code = negotiated ? "host_closing" : "unsupported_capability";
            return Json(self.welcome_ack);
        }
        self.welcome_ack = self.Select(selection);
        self.dirty = !self.welcome_ack.accepted;
        return Json(self.welcome_ack);
    }

    void Router::FinishWelcome(const bool queued)
    {
        auto& self = *impl_;
        std::scoped_lock lock(self.mutex);
        if (!queued) { self.SuspendLocked(); return; }
        self.Apply(self.welcome_ack);
        if (self.offered_revision != self.revision) self.dirty = true;
        self.welcome_ack = {};
    }

    void Router::Suspend()
    {
        auto& self = *impl_;
        std::scoped_lock lock(self.mutex);
        if (self.online || self.enabled) self.SuspendLocked();
    }

    bool Router::Negotiated() const
    {
        std::scoped_lock lock(impl_->mutex);
        return impl_->routing_envelope;
    }

    RoutingDiagnostic Router::Diagnostic() const
    {
        std::scoped_lock lock(impl_->mutex);
        return impl_->diagnostic;
    }

    Routed Router::Route(const ReceivedFrame& frame, Envelope& envelope)
    {
        if (!JsonObject(frame.json) || glz::read<EnvelopeOptions>(envelope, frame.json)) return Routed::Invalid;
        auto& self = *impl_;
        std::scoped_lock lock(self.mutex);
        if (frame.generation != self.generation || envelope.major != Major || envelope.client != self.client
            || envelope.session_id != self.session || envelope.pid != self.pid
            || envelope.process_started != std::to_string(self.started) || !IsIdentifier(envelope.type)
            || (envelope.recipient && !IsRecipient(*envelope.recipient))) return Routed::Invalid;
        if (!envelope.recipient || envelope.recipient == "gwrl") {
            if (!IsIdentifier(envelope.request_id)) return Routed::Invalid;
            if (!envelope.recipient && self.routing_envelope && envelope.type != "welcome") return Routed::Invalid;
            if (envelope.type == "routes_request") {
                if (!self.enabled || self.quiescing) return Routed::Handled;
                auto response = self.Message("routes_changed");
                response.request_id = envelope.request_id;
                response.routing = Json(self.OfferLocked());
                self.offer_id = response.request_id;
                self.refresh_json = Json(response).str;
                self.selection_json.clear();
                self.ack_json.clear();
                self.refresh_attempts = 1;
                self.refresh_deadline = GetTickCount64() + RefreshTimeout;
                self.dirty = false;
                self.Control(self.refresh_json);
                return Routed::Handled;
            }
            if (envelope.type == "routes_select") {
                if (!self.enabled || self.quiescing) return Routed::Handled;
                if (envelope.request_id == self.offer_id && !self.selection_json.empty()
                    && self.offered_revision == self.revision && self.selection_json == frame.json) {
                    self.Control(self.ack_json);
                    return Routed::Handled;
                }
                auto ack = self.Select(envelope.routing);
                if (self.offer_id.empty() || envelope.request_id != self.offer_id || !self.selection_json.empty()) {
                    ack.accepted = false;
                    ack.code = "stale_route_list";
                    ack.routes.clear();
                }
                auto response = self.Message("routes_ack");
                response.request_id = envelope.request_id;
                response.routing = Json(ack);
                const auto bytes = Json(response).str;
                if (self.Control(bytes) && ack.accepted) {
                    self.Apply(ack);
                    self.selection_json = frame.json;
                    self.ack_json = bytes;
                    self.refresh_json.clear();
                }
                return Routed::Handled;
            }
            if (envelope.type.starts_with("routes_")) return Routed::Handled;
            return Routed::Update;
        }
        if (!envelope.kind || !envelope.route_version || !*envelope.route_version
            || !envelope.route_session || !IsToken(*envelope.route_session)
            || !envelope.payload || !JsonObject(envelope.payload->str)
            || (envelope.code && envelope.code->size() > 64) || (envelope.detail && envelope.detail->size() > 1024)) return Routed::Invalid;
        const auto request = envelope.kind == "request", response = envelope.kind == "response", event = envelope.kind == "event";
        if ((!request && !response && !event) || (event ? !envelope.request_id.empty() : !IsIdentifier(envelope.request_id))) return Routed::Invalid;
        if (!self.handshake) { self.Error(envelope, "handshake_required"); return Routed::Handled; }
        if (!self.enabled) { self.Error(envelope, "unsupported_capability"); return Routed::Handled; }
        const auto found = std::ranges::find_if(self.registrations, [&](const auto& item) { return item.second->descriptor.recipient == envelope.recipient; });
        if (self.quiescing || found == self.registrations.end() || found->second->closing || !found->second->selection) {
            self.Error(envelope, "recipient_unavailable"); return Routed::Handled;
        }
        auto& route = *found->second;
        if (route.selection->route_version != envelope.route_version) { self.Error(envelope, "unsupported_route_version"); return Routed::Handled; }
        if (route.selection->route_session != envelope.route_session) { self.Error(envelope, "stale_route_session"); return Routed::Handled; }
        if (!response && (std::cmp_greater_equal(std::ranges::count_if(route.pending, [](const auto& work) { return work.message.kind != "response"; }), MaximumRouteQueue)
            || self.Queued() >= MaximumDispatch)) {
            self.Error(envelope, "route_busy"); return Routed::Handled;
        }
        auto context = uint64_t{0};
        if (request) {
            auto incoming = size_t{0};
            for (const auto& [id, pending] : self.requests) {
                if (pending.registration != route.handle || !pending.incoming) continue;
                ++incoming;
                if (pending.message.request_id == envelope.request_id) { self.Error(envelope, "request_in_progress"); return Routed::Handled; }
            }
            if (incoming >= MaximumRouteQueue) { self.Error(envelope, "route_busy"); return Routed::Handled; }
            context = ++self.serial;
            self.requests.emplace(context, Impl::Pending{route.handle, envelope, GetTickCount64() + RequestTimeout, true});
        }
        else if (response) {
            const auto pending = std::ranges::find_if(self.requests, [&](const auto& item) {
                return !item.second.incoming && item.second.registration == route.handle && item.second.message.request_id == envelope.request_id;
            });
            if (pending == self.requests.end()) return Routed::Handled;
            context = pending->first;
            self.requests.erase(pending);
        }
        route.pending.push_back({envelope, context});
        return Routed::Handled;
    }

    void Router::Pump()
    {
        auto& self = *impl_;
        const auto now = GetTickCount64();
        {
            std::scoped_lock lock(self.mutex);
            if (self.online && (!self.transport.Connected() || self.transport.Generation() != self.generation)) self.SuspendLocked();
            if (self.enabled && !self.quiescing && self.dirty) {
                auto offer = self.Message("routes_changed");
                offer.request_id = "routes-" + std::to_string(++self.serial);
                offer.routing = Json(self.OfferLocked());
                self.offer_id = offer.request_id;
                self.refresh_json = Json(offer).str;
                self.selection_json.clear();
                self.ack_json.clear();
                self.refresh_attempts = 0;
                self.refresh_deadline = now;
                self.dirty = false;
            }
            if (self.enabled && !self.quiescing && !self.refresh_json.empty() && now >= self.refresh_deadline) {
                if (self.refresh_attempts++ < 3) {
                    self.Control(self.refresh_json);
                    self.refresh_deadline = now + RefreshTimeout;
                }
                else {
                    self.Record("route_refresh_timeout", self.Message("routes_changed"));
                    self.refresh_json.clear();
                }
            }
            for (auto it = self.requests.begin(); it != self.requests.end();) {
                if (now < it->second.deadline) { ++it; continue; }
                const auto route = self.registrations.find(it->second.registration);
                if (it->second.incoming) {
                    if (route != self.registrations.end()) std::erase_if(route->second->pending, [&](const auto& work) { return work.request == it->first; });
                    self.Error(it->second.message, "request_timeout");
                }
                else if (route != self.registrations.end() && !route->second->closing) {
                    // Backpressure must not discard a completion that the caller is still waiting for.
                    if (route->second->pending.size() >= MaximumRouteQueue * 2 || self.Queued() >= MaximumDispatch) { ++it; continue; }
                    auto response = it->second.message;
                    response.kind = "response";
                    response.type = "error";
                    response.payload = glz::raw_json{"{}"};
                    response.code = "request_timeout";
                    response.detail = "No response arrived; the operation may have completed remotely.";
                    route->second->pending.push_back({std::move(response), it->first});
                }
                it = self.requests.erase(it);
            }
        }
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(2);
        for (auto count = size_t{0}; count < 16 && std::chrono::steady_clock::now() < deadline; ++count) {
            std::shared_ptr<Impl::Registration> route;
            Impl::Dispatch dispatch;
            auto notification = false;
            auto availability = GWRL_UNAVAILABLE;
            auto version = uint32_t{0};
            {
                std::scoped_lock lock(self.mutex);
                self.Sweep();
                if (self.registrations.empty()) break;
                auto next = self.registrations.upper_bound(self.cursor);
                for (auto attempts = self.registrations.size(); attempts; --attempts) {
                    if (next == self.registrations.end()) next = self.registrations.begin();
                    if (!next->second->closing && (next->second->notify || !next->second->pending.empty())) { route = next->second; break; }
                    ++next;
                }
                if (!route) break;
                self.cursor = route->handle;
                ++route->active;
                notification = std::exchange(route->notify, false);
                if (notification) {
                    availability = !self.online ? GWRL_OFFLINE : route->selection ? GWRL_AVAILABLE : GWRL_UNAVAILABLE;
                    version = route->selection ? route->selection->route_version : 0;
                }
                else { dispatch = std::move(route->pending.front()); route->pending.pop_front(); }
            }
            auto failed = false;
            try {
                if (notification) {
                    if (route->callbacks.on_availability) route->callbacks.on_availability(route->callbacks.user, availability, version);
                }
                else {
                    const auto& message = dispatch.message;
                    const auto empty = std::string();
                    const auto payload = message.payload ? message.payload->str : "{}";
                    const auto kind = message.kind == "request" ? GWRL_REQUEST : message.kind == "response" ? GWRL_RESPONSE : GWRL_EVENT;
                    const GwrlIncoming incoming{sizeof(GwrlIncoming), kind, dispatch.request, message.route_version.value_or(0),
                        Bytes(message.type), Bytes(message.request_id), Bytes(payload), Bytes(message.code ? *message.code : empty), Bytes(message.detail ? *message.detail : empty)};
                    route->callbacks.on_message(route->callbacks.user, &incoming);
                }
            }
            catch (...) { failed = true; }
            {
                std::scoped_lock lock(self.mutex);
                --route->active;
                if (failed) {
                    auto message = self.Message("callback");
                    message.recipient = route->descriptor.recipient;
                    self.Record("callback_exception", message);
                    self.Remove(*route);
                }
                self.Sweep();
            }
        }
    }

    void Router::Quiesce()
    {
        auto& self = *impl_;
        std::scoped_lock lock(self.mutex);
        if (self.quiescing) return;
        self.quiescing = true;
        for (const auto& [id, route] : self.registrations) if (!route->closing) self.Invalidate(*route, true);
        self.transport.CancelModule();
    }

    void Router::OpenOwner(const uintptr_t owner)
    {
        std::scoped_lock lock(impl_->mutex);
        impl_->closing_owners.erase(owner);
    }

    void Router::CloseOwner(const uintptr_t owner)
    {
        auto& self = *impl_;
        std::scoped_lock lock(self.mutex);
        self.closing_owners.insert(owner);
        for (const auto& [id, route] : self.registrations) if (route->callbacks.owner_module == owner) self.Remove(*route);
        self.Sweep();
    }

    bool Router::OwnerDrained(const uintptr_t owner) const
    {
        std::scoped_lock lock(impl_->mutex);
        return std::ranges::none_of(impl_->registrations, [&](const auto& item) { return item.second->callbacks.owner_module == owner; });
    }

    void Router::Close()
    {
        auto& self = *impl_;
        std::scoped_lock lock(self.mutex);
        self.closing = self.quiescing = true;
        self.SuspendLocked();
        for (const auto& [id, route] : self.registrations) self.Remove(*route);
        self.Sweep();
    }

    bool Router::Drained() const
    {
        std::scoped_lock lock(impl_->mutex);
        return impl_->registrations.empty();
    }
}
