---
title: "GWRL module routing"
description: "The module_routing_v1 wire contract and registration API for GWRLauncher and modules embedded in Toolbox or another host."
---

# GWRL module routing

This is the implementation contract for `module_routing_v1`. Give this document and the existing [GWRL update protocol](/docs/gwrl_protocol/) to the launcher maintainer. The launcher must implement the matching extension before module messaging is available. GWRL remains unversioned inside its host payload: it has no independent release, artifact revision, or manifest entry. No release-version increment accompanies this extension.

GWRLauncher selects a game-process/host connection. Within that connection, GWRL selects a registered recipient. Each feature owns its commands, payload schema, behavior, initialization, and retry policy. Adding commands or another recipient requires no additions to GWRL's command list. There is no automatic cross-process broadcast or module-to-module forwarding.

## Compatibility and ownership

- Keep envelope `major: 1`, `minor: 0`, the 528-byte bootstrap, `hold_plugins: 0`, framing, and existing Windows peer-identity checks.
- Toolbox keeps `client: "toolbox"` and the `.toolbox` bootstrap suffix. A different host supplies its own fixed adapter identity and corresponding discovery suffix; it does not select an identity from an incoming message.
- Reserve recipient `gwrl` for connection control, route negotiation, and the existing Toolbox update adapter. Other recipients cannot register or send as `gwrl`.
- Negotiate `module_routing_v1` explicitly. A newer minor number does not authorize routing. An older launcher can ignore the added hello fields and continue the existing update protocol. A new launcher connecting to an older Toolbox must keep using that baseline.
- Initial `hello` and `welcome` can use the baseline envelope without `recipient`. After a routing-enabled `welcome`, the participant's `status` and subsequent new control/update messages include `recipient: "gwrl"`. Renewed `welcome` may still use the baseline envelope. Feature messages always have an explicit recipient.
- The `gwrl` update vocabulary, inventory, transaction handling, and raw-byte request replay rules remain intact. Routing controls bypass the update replay cache and never change update UI, inventory, or transaction state. Feature replies never inherit Toolbox state or transaction fields.
- `route_version` describes the receiving feature's message contract. API `abi: 1` describes the registration function-table layout. Neither is a version of the GWRL module.

## Module envelope

Every frame is one length-prefixed UTF-8 JSON object using the existing named-pipe framing. This example is a launcher request to a registered Toolbox feature:

```json
{
  "major": 1,
  "minor": 0,
  "client": "toolbox",
  "session_id": "0123456789abcdef0123456789abcdef",
  "pid": 4321,
  "process_started": "134000000000000000",
  "type": "query_status",
  "request_id": "launcher-42",
  "recipient": "example_feature",
  "kind": "request",
  "route_version": 1,
  "route_session": "fedcba9876543210fedcba9876543210",
  "payload": {"include_details": true}
}
```

| Field | Requirement |
| --- | --- |
| Eight baseline fields | `major`, `minor`, `client`, `session_id`, `pid`, `process_started`, `type`, `request_id` are required |
| `pid`, `process_started` | Identify the game participant in both directions; FILETIME is a decimal string |
| `recipient` | 1–64 lowercase ASCII characters from `[a-z0-9_-]`; exact match; `gwrl` reserved |
| `type` | 1–64 ASCII characters from `[A-Za-z0-9_-]`; meaning belongs to the module |
| `kind` | Exactly `request`, `response`, or `event` |
| `request_id` | Nonempty identifier for requests/responses; empty string for unsolicited events |
| `route_version` | Positive integer selected from the registered feature's supported versions |
| `route_session` | 32 lowercase hexadecimal characters identifying this negotiated activation |
| `payload` | Required JSON object, including `{}` for an empty payload; nested unknown fields are preserved |
| `code`, `detail` | Optional response error information, at most 64 and 1,024 UTF-8 bytes respectively |

Explicit kind and route fields do not become requirements for existing `gwrl` messages. Routing controls use the baseline envelope plus `recipient: "gwrl"` and a `routing` object; they have no module `kind`, `payload`, `route_version`, or `route_session` requirement.

The bridge validates structure, size, and routing metadata. It does not decode feature commands. Module payloads must be valid UTF-8 JSON objects; the entire frame is limited to 65,536 bytes and 32 levels of object/array nesting. NUL bytes, a BOM before the object, invalid JSON, and invalid UTF-8 are rejected. Invalid process/session identity is never dispatched.

Responses retain the request's recipient, request ID, version, and activation token. Correlate operations by connection/session, recipient/activation, origin direction, and request ID. Different routes may use identical request IDs and command names. An unsolicited response is ignored, and an undeliverable response/event never generates an error loop.

## Initial negotiation

The participant advertises at most 32 currently registered routes. Each registration has a fresh random identity and 1–8 distinct positive supported versions. Registration means an endpoint exists; feature-specific readiness is part of that feature's contract.

The participant adds `module_routing_v1` to `hello.capabilities` and includes this `routing` offer:

```json
{
  "revision": "7",
  "routes": [
    {
      "recipient": "example_feature",
      "registration_id": "11111111111111111111111111111111",
      "versions": [1, 2]
    }
  ]
}
```

`revision` is the participant's monotonically increasing registration-list revision, encoded as a decimal string. It changes on registration or withdrawal and is unrelated to artifact versions. An empty list is valid.

A routing-capable launcher includes `module_routing_v1` in `welcome.capabilities`. It intersects the offer with its own currently registered handlers and places a full selection snapshot in `welcome.routing`:

```json
{
  "revision": "7",
  "routes": [
    {
      "recipient": "example_feature",
      "registration_id": "11111111111111111111111111111111",
      "peer_registration_id": "22222222222222222222222222222222",
      "route_version": 1,
      "route_session": "33333333333333333333333333333333"
    }
  ]
}
```

The launcher owns its handler registration identity and generates the activation token. Both identities and the token use 32 lowercase hexadecimal characters. Choose a mutually supported version; unselected destinations remain unavailable. A manifest entry is not evidence of a loaded handler.

The participant validates the whole selection atomically and includes this acknowledgement in the existing `status.routing`:

```json
{
  "revision": "7",
  "accepted": true,
  "code": "",
  "routes": [
    {
      "recipient": "example_feature",
      "registration_id": "11111111111111111111111111111111",
      "peer_registration_id": "22222222222222222222222222222222",
      "route_version": 1,
      "route_session": "33333333333333333333333333333333"
    }
  ]
}
```

The participant activates routes only after successfully queuing this acknowledgement. Its transport sends control frames before feature frames. The launcher activates routes only after receiving and validating the matching acknowledgement; it must not send feature requests speculatively after `welcome`.

An invalid/stale selection produces `accepted: false`, an error `code`, and no selected routes. This rejects routing negotiation without rejecting an otherwise valid Toolbox update handshake. The participant follows with a current route offer. Toolbox continues to require `cooperative_update_v1` and `normal_lifecycle_v1` for its update handshake; a future routing-only host can supply a different host adapter without pretending to support Toolbox updates.

## Registration changes while connected

All of these messages use recipient `gwrl`, a nonempty request ID, and no Toolbox transaction fields.

1. When its registration list changes, the participant sends `routes_changed` with the current offer in `routing`. Participant-generated request IDs use `routes-<sequence>`.
2. The launcher sends `routes_select`, echoes the `routes_changed.request_id`, and supplies a full selection snapshot in `routing`.
3. The participant sends `routes_ack`, echoes that request ID, and supplies the same acknowledgement structure as `status.routing`.
4. The launcher applies the selection after the accepted acknowledgement. Unselected routes are revoked. An unaffected route keeps exactly the same registration identities, version, and activation token, so unrelated pending work survives.

When the launcher's handlers change, it first sends `routes_request`. The participant answers with `routes_changed` using that request ID, followed by steps 2–4. The launcher must revoke a removed local handler immediately rather than waiting for negotiation to finish.

Only the latest participant offer can be accepted. The revision and both registration identities must still match. Registration changes can supersede an outstanding offer. Delayed acknowledgements and traffic for an earlier registration cannot reach its replacement, even when the recipient text is reused.

For an unchanged pair of registrations and selected version, retain the current activation token. Replacing either registration, changing the selected version, or renewing the connection handshake requires a fresh token. The participant rejects previously used tokens within a session, except for retaining an unchanged active route. Up to 4,096 activation tokens are remembered per session; reaching that bound requires a fresh connection session before creating further activations, without altering an existing update transaction.

The participant sends the same outstanding offer up to three times, five seconds apart. After that, existing unaffected routes remain usable; unnegotiated destinations remain unavailable. A subsequent list change or `routes_request` starts another exchange. A duplicate byte-identical `routes_select` for the current unchanged offer repeats the cached `routes_ack`; changed or stale selections are rejected. This one routing acknowledgement cache is independent of Toolbox's update replay cache.

Selection error codes include `invalid_route_selection`, `stale_route_list`, `unsupported_route_version`, `route_session_changed`, `stale_route_session`, `routing_session_exhausted`, and `host_closing`.

## Failures, reconnects, and timeouts

For a well-formed module request that cannot be delivered, GWRL returns a response with `type: "error"`, `kind: "response"`, `payload: {}`, the original routing/correlation fields, and a `code`/`detail`. Delivery errors include:

- `handshake_required`: no current completed handshake.
- `unsupported_capability`: the peer did not negotiate routing.
- `recipient_unavailable`: the destination is missing, unselected, or closing.
- `unsupported_route_version` or `stale_route_session`: wrong negotiated context.
- `route_busy`: the destination's dispatch/request allowance is full.
- `request_in_progress`: that route already has an incoming request with this ID awaiting completion.
- `request_timeout` or `request_cancelled`: the pending reply expired or was explicitly released.

A receiving module defines errors for unknown commands or invalid feature payloads. Such errors do not become Toolbox update errors.

Both directions allow at most eight pending requests per registered route. Bridge-managed requests expire after 30 seconds. Timeout is not proof that a remote action failed to execute. Longer operations should acknowledge receipt and use a module-defined operation ID and later event. The bridge does not automatically replay feature requests on timeout or reconnect. Feature contracts define any safe retry/idempotency policy.

Disconnect, heartbeat expiry, renewed welcome, or changed route activation revokes old incoming reply contexts and notifies registered owners. Pending outbound requests receive local failure completions; a reply already received may still complete locally. No expired context can send into a newer connection. Existing five-second heartbeats and 15-second liveness checks remain in effect. New negotiation uses fresh activation tokens. Liveness suspension revokes authorization but retains the negotiated envelope format until the connection is replaced or a renewed welcome changes the capability selection.

## Registration API and embedding

Public header: `Dependencies/GWRL/include/GWRL/Api.h`.

Toolbox exports the undecorated C function `GWRL_GetApi(uint32_t abi)`. Resolve it from the Toolbox module handle already passed to a plugin's `Initialize`; request ABI `1`. The function returns a borrowed immutable `GwrlApi` table, or null for an unsupported ABI. The API table and all handles are valid only while that host DLL is loaded. Discard them on full host unload/reinjection and resolve the API again during normal startup.

The interface uses `__cdecl`, size-tagged structures, opaque integer handles, and explicit byte lengths. It passes no owning STL objects, JSON-library objects, or C++ exceptions across DLL boundaries. Use the header's default platform packing. `owner_module` must be the module handle containing the callback functions; the host validates callback ownership.

| API operation | Behavior |
| --- | --- |
| `register_route` | Copies recipient and versions, records callbacks/owner, returns a registration handle |
| `send_request` | Copies type/payload, stamps the envelope, returns a local request handle for its response |
| `emit_event` | Copies type/payload and sends an event with an empty wire request ID |
| `reply` | Replies using an incoming request handle; works asynchronously until expiry/revocation |
| `release_request` | Releases an incoming request without feature execution/reply and reports cancellation where possible |
| `begin_unregister` | Immediately prevents further dispatch admission and sending for this registration |
| `is_drained` | Reports that unregistration has finished and no callback for that registration is executing |

API calls are thread-safe and do not wait for pipe I/O or callback completion. They may acquire short internal mutexes. `GWRL_QUEUED` means local acceptance only; it does not prove remote receipt or completion. Other outcomes are invalid argument, duplicate route, disconnected, not negotiated, closing, oversized, queue full, expired context, and internal failure.

On a request callback, `GwrlIncoming.request` is the opaque reply context. On a response callback, it is the local handle returned by `send_request`. Events have zero. `request_id` exposes the wire correlation identifier if the feature needs it. Callback byte views are borrowed for that invocation only; copy them before scheduling asynchronous feature work. The integer reply context itself can be retained without retaining DLL-owned bridge objects. Replying consumes it only after a successful enqueue; a queue-full result leaves it available for retry until expiry.

Callbacks execute through the host's pump outside all router/transport registry locks. Toolbox calls that pump on its normal game update thread, after releasing GWRL's UI/state mutex. Callbacks may reply, send, or begin unregistering themselves. They must return promptly, must not throw across the C boundary, and must not block waiting for their own drain barrier. Features own any additional synchronization with their rendering or background work.

A plugin can acquire and register the API during its existing initialization:

```cpp
const auto get_api = reinterpret_cast<GwrlGetApiFn>(GetProcAddress(toolbox_module, "GWRL_GetApi"));
const auto api = get_api ? get_api(1) : nullptr;
if (!api || api->size < sizeof(GwrlApi)) return;
const uint32_t versions[] = {1};
const GwrlRouteConfig route{
    sizeof(GwrlRouteConfig), {"example_feature", 15}, versions, 1,
    reinterpret_cast<uintptr_t>(this_plugin_module), this,
    &OnMessage, &OnAvailability
};
GwrlRegistration registration = 0;
const auto result = api->register_route(api->context, &route, &registration);
```

`OnMessage` and `OnAvailability` must use the callback signatures and calling convention in `Api.h`. Keep the table and successful registration handle in the feature's own state. Poll its drain barrier through normal termination, without blocking that callback thread.

The `user` pointer must remain alive until `is_drained` succeeds. Unregistration cancels queued dispatch and invalidates retained request handles immediately; executing callbacks are allowed to return. A retained integer handle does not keep a plugin loaded. The host must also drain work scheduled outside GWRL through that plugin's normal termination logic. A timeout never authorizes forced DLL release.

Toolbox automatically closes all routes owned by a plugin DLL and waits for active routing callbacks before entering its existing settings-save/termination sequence. Reload initialization reopens registration for that DLL. A built-in module or independently enabled child feature must unregister its own handle when it stops; unloading an entire plugin DLL closes all its child routes.

The reusable target is `GWRL::Core`, built by `Dependencies/GWRL/CMakeLists.txt`. It links statically into its host, requires Windows and the Glaze headers, and has no Toolbox, GWCA, Qt, or ImGui dependency. The C++ host API in `Router.h` binds it to a transport and the host's existing startup/update/shutdown integration. Its adapter must drive session changes, hello/welcome acknowledgement, liveness suspension, and the safe-thread pump. No extra runtime DLL or publishing manifest is introduced.

## Queue and shutdown guarantees

- Each transport direction retains its existing 64-message bound. Module traffic is limited to 48 queued messages total and eight per route, leaving capacity for control traffic.
- Control sends have priority. Feature output rotates between active registrations. Incoming control frames are processed before queued feature frames.
- Each registered destination has space for up to eight ordinary dispatches and eight outbound completions. Outbound request admission accounts for completions waiting to be delivered, so event traffic cannot consume their allowance.
- Each Toolbox pump invokes at most 16 callbacks and starts no further callbacks once its two-millisecond budget expires. A callback already running is not preempted. Ready destinations rotate fairly.
- Local over-budget sends return `GWRL_QUEUE_FULL`. Incoming transport overflow or malformed frames closes the connection, as in the baseline transport; peers must honor admission bounds and pace producers.
- Closing a route removes its not-yet-started queued sends. A pipe write already in progress may complete; no API promises that an accepted remote command was cancelled. Old-session frames and reply handles cannot be replayed into a new connection.

On a full Toolbox update, GWRL stops accepting module work before queuing `accepted` and `shutdown_starting`. It waits for the specific shutdown control frame to finish writing, rather than requiring unrelated module traffic to empty the transport. It then invokes the existing normal Toolbox exit path. Route callback drain remains part of DLL lifetime safety; config/state preservation remains owned by normal Toolbox/plugin lifecycle code.

Plugin-only updates close only routes owned by the affected DLLs through the existing selective unload path. Other routes and the update transaction continue. Reloaded plugins register fresh destinations during normal initialization and negotiate fresh activations. The launcher still replaces only outdated artifacts and reloads only plugins that the participant previously had loaded.

Routing failures are recorded with bounded recipient/type/request metadata and a cumulative count. Toolbox logs at most one such diagnostic every five seconds. Payload contents are not logged.

## Validation

Regression tests are local-only; no tests are added to publishing workflows. Test the core with fake peers and the actual Toolbox GWRL adapter with a simulated host. Required checks include routing/type isolation, nested payload preservation, old/new peer compatibility, stale registrations and replies, refresh during updates, callback/unload races, bounded queues, and normal update shutdown. Passing simulation tests does not establish that the launcher implements this extension or replace live-client update/reinjection checks.
