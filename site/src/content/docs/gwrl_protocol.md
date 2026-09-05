---
title: "GWRL communication protocol v1"
description: "The Windows IPC contract for GWRLauncher and Toolbox: discovery, normal Toolbox restart, selective plugin reload and file-update recovery."
section: features
---

# GWRL communication protocol v1

Status: **Toolbox implementation included; launcher integration required.** The bridge implements the finalized `normal_lifecycle_v1` contract below. The launcher must implement this contract and complete live-client integration testing before enabling coordinated updates.

Transport/envelope version remains 1.0. The required capability `normal_lifecycle_v1` distinguishes these semantics from the earlier bridge; `cooperative_update_v1` alone is insufficient. A controller must not attempt this lifecycle with a peer that does not advertise it. The wire contract below is shared by both implementations.

## Scope and ownership

GWRLauncher is the controller. Each Guild Wars process containing a compatible GWRL module is a participant. The controller owns discovery of affected processes, downloads, entitlement/authenticity checks, staging, file backups, replacement, binary rollback, reinjection, and the durable file-transaction journal.

Toolbox owns its configuration, plugin settings and lifecycle state. A full Toolbox update uses the existing normal exit and startup paths to save and restore that state. A plugin-only update uses the existing plugin save/unload/load paths and remembers, within each surviving Toolbox instance, which selected plugins were actually loaded before the update. The launcher does not serialize or restore Toolbox's internal state or require a complete plugin recovery inventory.

State preservation has the same meaning as a normal Toolbox/plugin shutdown and startup. This protocol does not introduce a separate snapshot of transient game/plugin memory or promise restoration beyond the existing save/load behavior.

An update MUST follow an explicit user action in the launcher or the participant's **Update now** button. Availability notification alone MUST NOT unload anything. The launcher MUST authenticate/verify downloaded artifacts and stage every selected artifact before asking any participant to prepare. No download URL or executable filesystem command is accepted by this bridge.

In-place update is unsupported unless **every affected live process** has a compatible bridge, has completed the handshake, and is responding. An old Toolbox without a bridge, a disconnected bridge, an unknown consumer of the target file, or an unreadable process inventory blocks replacement. Expected disconnection after authorized Toolbox shutdown is an exception: the controller must verify module absence independently.

This revision supports only `client: "toolbox"`. PotatoBox is deferred; its installation and live-update adapter are not part of this pass.

### Managed artifacts and locations

The backend reports both categories as `plugins`. GWRLauncher classifies them through its supported backend-key registry, not by matching arbitrary registered DLL filenames.

| Category | Managed location | Behavior |
| --- | --- | --- |
| Injection mod: GWRL's `GWToolboxdll.dll` | `<gwrl_install_dir>/mods/GWToolboxdll.dll` | Register the installed managed copy in the launcher's DLL list; use full Toolbox exit/reinjection when replacing a live copy |
| Toolbox plugins: currently `DBBox.dll` and `SCTracker.dll` | `<Documents>/GWToolboxpp/<machine_name>/plugins/<plugin>.dll` | Use selective plugin unload/reload when Toolbox itself is not being updated |

Both categories appear in the Toolbox Features table. Other user-registered Toolbox copies, including copies with the same filename, remain distinct and are not replaced or re-registered as managed artifacts. A process mapping a shared target plugin file must still be accounted for before that file can be replaced, regardless of which Toolbox copy hosts it.

## Discovery and process identity

The controller creates a per-participant, local, duplex **message-mode Windows named pipe**:

```text
\\.\pipe\GWRL.v1.<unpredictable unique suffix>
```

Use `PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS`, overlapped I/O and `FILE_FLAG_FIRST_PIPE_INSTANCE`. Do not use Qt's default byte-stream local socket without implementing these exact Windows message boundaries. Use one connection per process/adapter.

Publish its name through a Windows file mapping:

```text
Local\GWRL.Bootstrap.v1.<decimal game PID>.toolbox
```

Use a DACL restricted to the current logon/user and SYSTEM for both objects. Never use a NULL/world-writable DACL. The bridge checks that the server PID equals the declared controller PID, its creation time matches, its token user SID matches the game's user SID, and both processes belong to the same Windows session. The controller MUST symmetrically verify the connected client with `GetNamedPipeClientProcessId`, its creation time and user/session before accepting messages.

The session token and restrictive ACL protect endpoint association and accidental cross-session use. This is a **same-Windows-user trust boundary**, not authentication against malicious code already running as that same user. Do not run the pipe with impersonation privileges: the bridge opens it with `SECURITY_IDENTIFICATION`.

The controller keeps the mapping alive while that endpoint is valid. Publish a complete immutable descriptor before setting `published = 1` with release ordering. Keep its remaining bytes unchanged; replace the mapping/endpoint deliberately on reconnect. The bridge polls roughly once per second. A PID alone never identifies a participant: pair it with its process creation FILETIME.

### Bootstrap binary layout

All integer fields are unsigned, little-endian. Exact size: **528 bytes**, compatible with Win32 and Win64 Windows. No pointers, C++ bools or platform-dependent `size_t` values occur on the wire. Clear all padding and unused string bytes.

| Offset | Bytes | Field | Value |
| ---: | ---: | --- | --- |
| 0 | 4 | magic | `0x4c525747` (bytes `47 57 52 4c`, GWRL) |
| 4 | 2 | major | `1` |
| 6 | 2 | minor | `0` |
| 8 | 4 | size | `528` |
| 12 | 4 | published | `1` only after descriptor is complete |
| 16 | 4 | controller_pid | Launcher PID |
| 20 | 4 | target_pid | Game PID |
| 24 | 8 | controller_started | `GetProcessTimes` creation FILETIME as uint64 |
| 32 | 8 | target_started | Game creation FILETIME as uint64 |
| 40 | 33 | session_id | Exactly 32 ASCII identifier characters, then NUL |
| 73 | 65 | transaction_id | Empty for an ordinary connection; update correlation ID for reinjection, up to 64 ASCII identifier characters, then NUL |
| 138 | 2 | reserved | Zero |
| 140 | 4 | hold_plugins | Always `0` in this lifecycle; retained in the binary layout for compatibility |
| 144 | 384 | pipe_name | 192 UTF-16LE code units, NUL terminated |

Identifier characters are `[A-Za-z0-9_-]`. Generate a cryptographically random 128-bit session value encoded as 32 lowercase hex digits. For update-driven reinjection, publish a fresh valid descriptor before injecting, with the same transaction ID and `hold_plugins = 0`. Keep it alive until reconnection and completion can be observed.

The transaction ID correlates the restarted Toolbox with the launcher's file transaction. It is not a plugin selection or recovery snapshot. Toolbox follows normal startup and plugin autoload without waiting for a launcher `commit_update` or complete inventory. The earlier `hold_plugins = 1` recovery path is not used by this revision.

## Framing and JSON

One pipe message contains one complete frame:

```text
uint32_le UTF8_byte_count
UTF8 JSON object, exactly UTF8_byte_count bytes
```

The length excludes its four-byte prefix. Valid payload lengths are 1–65,536 bytes. Send prefix and payload together in one `WriteFile`, not two messages. UTF-8 has no BOM or trailing NUL. Do not use native Qt `QDataStream` string framing or UTF-16 JSON. Invalid length, message mode, queue overflow or I/O failure disconnects the transport. Each direction has a bounded 64-message queue. Individual pending reads/writes time out after approximately two seconds and are cancelled on shutdown.

For example, the two-byte payload `{}` has frame bytes `02 00 00 00 7b 7d`; it passes framing but does not pass the message envelope checks.

### Envelope

Both directions use this object shape. Requests MUST include the first eight fields below. The bridge emits optional fields with empty/default values. Unknown JSON fields are ignored to allow additive minor revisions. Major revisions must match; consumers must not infer support for new operations from a higher minor number.

```json
{
  "major": 1,
  "minor": 0,
  "type": "get_inventory",
  "client": "toolbox",
  "session_id": "0123456789abcdef0123456789abcdef",
  "request_id": "launcher-1",
  "pid": 4321,
  "process_started": "134000000000000000",
  "transaction_id": "",
  "user_initiated": false,
  "state": "",
  "detail": "",
  "code": "",
  "capabilities": [],
  "artifacts": []
}
```

`pid` and `process_started` always identify the **game participant**, even in a controller message. The FILETIME is a decimal **string** to avoid JSON/JavaScript precision loss. `request_id`, `transaction_id` and `type` use identifier characters, with a maximum of 64 characters. Requests require a nonempty request ID. Unsolicited participant events, including `hello`, may have an empty request ID. Responses echo the request ID. Transaction commands must use the exact prepared transaction ID.

`state` is authoritative participant state; `detail` is human-readable UI information, not a machine discriminator. `code` is the error discriminator. The controller must tolerate additional error codes and stop the operation on unknown errors.

### Artifact object

```json
{
  "name": "DBBox.dll",
  "version": 2,
  "abi": 1,
  "sha256": "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
  "path": "C:\\Users\\Example\\Documents\\GWToolboxpp\\PC\\plugins\\DBBox.dll",
  "state": "loaded",
  "enabled": true
}
```

`name` is the adapter's canonical artifact ID. Current update plans accept only `DBBox.dll`, `SCTracker.dll` and `GWToolboxdll.dll`, with no duplicates and at most three entries. A plan requires a positive uint32 integer `version` and a 64-character lowercase hexadecimal `sha256`. DBBox and Toolbox require an explicit compatible `abi`. SCTracker has no new ABI/export requirement and is not modified by this implementation.

Protocol inventory is limited to the managed Toolbox core and supported backend plugins; the controller intersects these entries with its current backend catalog. Unsupported plugins are not required in the launcher inventory, transaction journal or recovery messages. Toolbox still handles all of its plugins internally through its normal full shutdown/startup path.

`version = 0` or `abi = 0` means unknown, not a version assertion. The bridge obtains DBBox's embedded metadata and may obtain another supported plugin's version from a hash-matching `.version.json` sidecar. Do not assume SCTracker has a new Windows version resource. Toolbox's integer fork revision, not its upstream/display version string, is the value used in update plans.

Inventory paths are actual participant paths encoded as UTF-8. Commands **never** choose load paths; the participant looks up its discovered managed inventory. The controller MUST resolve paths, shortcuts/hard links and physical file identity itself to group every consumer of the same target file. Never derive shared-file safety from a filename match alone. Plugin state is `loaded`, `loading`, `unloading` or `unloaded`; Toolbox is reported as loaded until it exits. `enabled` is the persisted plugin choice and must not be changed just because the update temporarily unloads a plugin.

For plugin-only updates, the participant remembers actual pre-update loaded state separately from `enabled`. Only selected plugins that it actually unloaded are eligible for reload. Full Toolbox startup instead follows Toolbox's ordinary persisted selection/autoload policy.

## Handshake, notifications and liveness

1. Participant connects and sends `hello`, with managed inventory, current transaction/state, a human-readable Toolbox display/build version and capabilities: `cooperative_update_v1`, `normal_lifecycle_v1`, `plugin_reload`, `toolbox_unload`.
2. Controller verifies the envelope/process identity and required capabilities, then replies `welcome` with `capabilities: ["cooperative_update_v1", "normal_lifecycle_v1"]` and a request ID.
3. Participant returns `status`. Only then may this connection participate in updates.
4. Either side may send `ping`; reply `pong` with the matching request ID. The participant pings every five seconds and suspends authorization after 15 seconds without a valid controller message. The controller must enforce its own liveness deadline too.

A `welcome` reestablishes the handshake after liveness failure. Physical reconnect sends a new `hello`. Prefer a fresh session ID and pipe after controller restart and inspect `query_transaction` before continuing. Never start an overlapping transaction.

After update-driven reinjection, `hello` carries the descriptor's transaction ID and `state: "starting"`, or `"ready"` if normal startup has already completed. Following the handshake, GWRL reports `ready` only after normal Toolbox startup and its scheduled plugin initialization have completed successfully. A startup error is reported as `error`/`startup_failed`; connection alone is not proof of successful restart. No commit command is required to begin startup. `query_transaction` must expose this progress if an event is missed.

Controller sends `updates_available` with a valid plan-shaped artifact list, or an empty list to clear it. Participant acknowledges with `ack`; a changed nonempty list opens the dismissible notification. This does not reserve or unload anything.

The in-game button sends `update_request` with `user_initiated: true` and the displayed artifacts. This is a **request to the launcher to stage and coordinate**. It does not authorize file replacement independently of the normal transaction. The controller may acknowledge with `ack`. It must revalidate the catalog, affected accounts and hashes rather than blindly trusting the notification's old list. Requests issued directly from the launcher use its normal user confirmation/UI action.

## Transaction messages

`type` is the response/event name; `state` is the participant's current phase. They are not interchangeable. In particular, cancelling preparation replies with `type: "ready"` and `state: "idle"`.

| Controller command | Required state | Participant result |
| --- | --- | --- |
| `get_inventory` | Handshaken | `status` with managed inventory |
| `query_transaction` | Handshaken | `status` with transaction/state/managed inventory |
| `prepare_update` | `idle`, no unfinished transaction | `prepared` with state `prepared`, or `error` |
| `begin_unload`, plugin-only plan | `prepared`, same transaction | `accepted` with state `unloading`, then `released` |
| `begin_unload`, full Toolbox plan | `prepared`, same transaction | `accepted`, then `shutdown_starting` with state `shutting_down`; normal Toolbox exit follows |
| `commit_update` | `released`, plugin-only plan | `accepted` with state `reloading`, then `ready`, or `error` |
| `finish_update` | `ready`, or `idle` after a cancelled preparation | `ack`; clears transaction/reservations and returns to `idle` |
| `rollback_update`, plugin-only plan | `ready` | `accepted`; releases the newly loaded selected plugins again, then `released` |
| `rollback_update`, restarted Toolbox | `starting`, `ready` or `failed`, same reinjection transaction | `accepted`, then `shutdown_starting`; normal Toolbox exit follows |
| `abort_update` | `prepared` | Cancels reservation without unloading; `ready` response with state `idle` |
| `abort_update`, plugin-only plan | `released` | Verifies restored original files, starts normal reload of its prior loaded set, then `ready` |

The participant states exposed by `hello`, `status` and events are:

| State | Meaning |
| --- | --- |
| `idle` | No active lifecycle operation; a cancelled preparation may still await `finish_update` before a new transaction |
| `prepared` | Approved plan reserved; no unload has begun |
| `unloading` | Selected plugins are draining through their normal unload paths |
| `released` | Selected plugin DLLs are released; waiting for verified replacement or restored originals |
| `reloading` | Previously loaded selected plugins are being initialized and their settings loaded |
| `shutting_down` | Normal full Toolbox exit is in progress; the bridge will disappear with Toolbox |
| `starting` | Reinjected Toolbox is running its normal startup/autoload logic |
| `ready` | Plugin reload or normal Toolbox restart completed; awaiting transaction completion |
| `failed` | Reinjected Toolbox reported a normal-startup failure; do not declare success or replace mapped files |

`prepare_update` requires a nonempty transaction ID, `user_initiated: true` and the verified, staged target plan in `artifacts`. A participant validates supported artifact identities, paths, hashes and applicable ABI requirements. Plugin-only plans reserve the selected plugins and remember their actual loaded state locally. Full Toolbox plans reserve the normal full-exit operation; they do not first run a separate GWRL-managed unload/recovery of every plugin. A busy/stopping component may reject preparation. Nothing is unloaded at `prepared`.

During a plugin reservation, manual operations that could load or unload the selected plugins are held. During a full Toolbox reservation, new manual lifecycle operations are held while normal shutdown is coordinated. Reservations must not alter saved enabled selections. After reinjection, normal startup/autoload is allowed; the transaction association only prevents an overlapping update until completion.

Existing save/unload/load logic is the source of lifecycle behavior. A plugin shutdown saves settings, signals termination, waits for the existing game-thread barrier and termination readiness, then releases the plugin manager's DLL reference. If another reference still maps the image, the participant remains `unloading`; it must not repeatedly release a reference it does not own. Update ticks continue when the existing shutdown path needs them to drain pending work.

This revised full Toolbox path does not use `shutdown_ack`, `shutdown_pending`, startup holds, `recovering`, or a complete-inventory `commit_update`. Do not mix commands from the earlier lifecycle into this one.

### Plugin-only update sequence

Use this path when the participant's Toolbox core is not being replaced. An explicit action in the launcher and an in-game GWRL `update_request` enter the same coordination flow.

1. Determine the selected, installed, outdated backend-supported plugins. Download, verify and stage all their replacements before any unload. Write the file-transaction journal with target file identities/paths, original and target hashes, version/ABI metadata, backup paths, participant PID/creation identities, and phase. It does not contain a complete Toolbox plugin inventory or plugin settings.
2. Hold launcher-controlled launches/injections that could load the affected files. Discover and prepare every affected Toolbox instance, including instances that use the shared plugin location but currently have a selected plugin unloaded. Each instance locally remembers which selected plugins it actually had loaded. On refusal, abort successful preparations and leave installed files alone.
3. Send `begin_unload` with the prepared transaction ID. Each instance invokes normal save/unload logic only for selected plugins it has loaded; an already-unloaded selected plugin is a no-op. Unselected plugins and Toolbox itself stay running. Temporary unloading must not persist a disabled selection.
4. Wait for `released` from every affected instance. This means that instance is ready for replacement, not that the update is complete. Independently verify that no process maps any target file. An unknown/unresponsive consumer blocks replacement.
5. Replace **only the selected outdated plugin files** at their established `Documents/GWToolboxpp/<machine_name>/plugins` paths. Keep recoverable originals and verify every installed hash before permitting any reload. Up-to-date, unselected and unsupported plugins are not replaced. If matching metadata sidecars are installed, include them in the same file transaction.
6. Send `commit_update` with the same transaction ID to all participants. A replacement plan is unnecessary here: the surviving bridge retains its prepared target hashes and its own prior loaded set. Each participant verifies the updated selected files and uses normal plugin initialization/settings loading **only for plugins that it had to unload**. A selected plugin that was previously unloaded stays unloaded, regardless of what another instance reloads.
7. Wait for `ready` from all participants, then send `finish_update` to all and wait for `ack`. Only then finish the journal and release the launch/load barrier. Until then, participants retain the selected-plugin reservation and may be asked to release a partially successful update for rollback.

If verification or reload fails, the participant reports an error rather than claiming readiness. If some new selected plugins already loaded, drain those instances through the same normal unload path and return to `released` before the launcher can restore files.

### Full Toolbox update sequence

Use this path whenever the selected set includes the managed `GWToolboxdll.dll`. Selected outdated supported plugins may be replaced in the same transaction. Do not run the plugin-only reload sequence first or restore plugins through a separate launcher-owned snapshot.

Only instances mapping the managed core being replaced are full-exit/reinjection targets. If a selected shared plugin also has consumers hosted by an unrelated Toolbox copy, coordinate those consumers with plugin-only plans under the same file transaction and release barrier. Reload their prior loaded subsets after replacement; do not replace their core or reinject the managed Toolbox into them. If those consumers cannot be coordinated, block the live update. Never treat a filename match as authorization to switch Toolbox copies.

1. Download, verify and stage the entire selected replacement set. Journal the managed target files, backups, and the exact live Toolbox instances to be restarted. Prepare every affected instance and prevent new launcher-controlled consumers from loading these files.
2. Persist the shutdown phase before sending `begin_unload`. In the full Toolbox branch, GWRL acknowledges the request, emits `shutdown_starting` and flushes that notification before entering **Toolbox's existing normal full-exit path**. That path owns saving configuration/settings, shutting down plugins, draining callbacks and unloading Toolbox. Do not substitute forced exported termination, direct repeated `FreeLibrary` calls, or a second state-save system.
3. The launcher independently confirms that the managed Toolbox DLL and every other target DLL being replaced are absent from all affected processes. Pipe disconnection alone is not proof of release. Keep Guild Wars processes running; do not terminate games to force an update. A minimized client/device loss or pending callback may delay normal exit; warn/wait or stop safely.
4. After every target file is free, replace and verify the entire selected artifact set. Do not reinject any instance while part of the set is still being copied or has failed verification. Only managed, selected outdated artifacts are replaced; unrelated registered injection mods and unsupported plugins remain untouched.
5. Publish ordinary bootstrap descriptors with fresh session IDs, the same file-transaction ID, and `hold_plugins = 0`. Reinject the updated managed Toolbox DLL only into the original, still-running game processes whose Toolbox instances unloaded for this transaction. Do not inject into a new game or one that did not previously have this Toolbox loaded.
6. Toolbox performs its normal startup and restores its own persisted configuration/plugin selections through its existing logic. This includes normal handling of unsupported plugins without exposing their inventory to the launcher. GWRL reconnects and handshakes, reports normal startup progress, and emits `ready` on successful completion. The launcher checks the managed core's hash/integer revision/ABI and the selected managed artifact results. It sends **no complete plugin inventory and no `commit_update` to initiate startup**.
7. After all restarted instances and any plugin-only participants are ready, send `finish_update` and collect acknowledgments. Complete the file journal, retire the reinjection transaction descriptors without mutating published bytes, and release the launcher barrier. Continue ordinary bridge discovery/monitoring.

If a game exits during the transaction, remove that restart target only after verifying its PID/creation identity and exit. A new process reusing its PID is not a continuation. If there were no live consumers initially, update files without inventing any reinjection targets.

### First-time installation

A backend-supported artifact whose managed destination does not exist is a first-time install, not an update of an already-discovered plugin. Stage/verify it, then install it at its managed location without overwriting an existing file. If the target appears before installation, re-inventory and treat it as an existing-artifact operation instead.

Register a newly installed managed injection mod in the launcher's DLL list without changing selections for other registered copies. A newly installed Toolbox plugin does not require unloading or notifying running Toolbox instances: they may discover it through their existing refresh/startup logic later. Installation alone does not request a load/injection or enable a new feature. A plugin that has appeared on disk but is not yet in a live participant's inventory must be re-discovered before it can participate in a later live update.

## Cancellation, failures and reconnects

The controller journal covers **file operations and process coordination**, not Toolbox/plugin configuration. It must be durable before an unload, replacement, rollback or reinjection can leave the system changed. On controller restart, reconcile pending file transactions before permitting launcher-controlled launches/injections into affected files.

Before `begin_unload`, `abort_update` cancels preparations without unloading. For a full Toolbox update after normal exit has begun, do not attempt to interrupt that shutdown with the earlier `shutdown_ack`/held-recovery flow.

For a plugin-only transaction after release, first restore the original target files from backups and verify their hashes, then send `abort_update`. Each surviving Toolbox instance uses its own retained pre-update loaded set and normal load/settings logic. If another participant already reported `ready`, first send it `rollback_update` and wait until every target file is released everywhere before restoring backups. Finish/ack the recovered transaction after all participants are ready.

For a full Toolbox transaction, never restore a file that a newly reinjected Toolbox/plugin has mapped. If a partial restart must be rolled back, request normal full exit from any restarted participants using `rollback_update` with the reinjection transaction ID, and independently verify module absence again. Restore/verify the original managed artifact set and reinject the original Toolbox normally into the surviving recorded targets. The same ordinary startup and settings restoration applies; there is no complete-inventory `abort_update` or startup hold. Binary rollback does not rewind Toolbox configuration beyond its normal save/load and compatibility behavior.

If a full transaction also has plugin-only participants hosted by unrelated Toolbox copies, apply the plugin-only rollback rules to those participants. Every target file must be released across both groups before restoring any shared files; the unrelated cores stay running.

Disconnect and timeout never grant replacement permission, force unloads, or cause blind retries, automatic new update transactions or automatic rollback. Selected plugins already released remain stopped until their surviving Toolbox instances receive a verified commit/abort. Reconnect, handshake, query the transaction and reconcile actual file/module identities with the controller journal. If a Toolbox instance disappears during a plugin-only update, its transient prior-loaded set is not reconstructed by the launcher. Recover the files safely and use Toolbox's normal startup behavior if it is later restarted.

The launcher can block its own launches/injections; it cannot promise control over programs or injection tools started outside it. Recheck consumers immediately before replacing a file. A newly appearing or unknown consumer, a failed file replacement, or lost transaction state stops progress and retains recoverable backups. Offer a wait/recover or cold-update path after affected clients/Toolbox instances have exited; do not kill an active game or force library release.

The bridge has no forced deadline for normal shutdown or initialization. Suggested launcher UI thresholds are 30 seconds to warn during preparation/unload and 60 seconds for reinjection/startup, with a clear recover/wait option. Continue heartbeats while the bridge is alive. These are reporting thresholds, not permission to overwrite mapped files.

Important errors include `handshake_required`, `unsupported_capability`, `explicit_request_required`, `invalid_plan`, `unsupported_artifact`, `inventory_unreadable`, `plugin_busy`, `transaction_busy`, `wrong_transaction`, `invalid_state`, `core_build_mismatch`, `restore_failed`, `startup_failed`, `request_id_reused`, `reconnect_required` and `unknown_message`. Invalid envelopes or JSON are discarded; do not rely on an error response to malformed input.

### Idempotency

For transaction commands, retry the **identical JSON payload bytes** with the same request ID in the same session. The participant caches up to 128 command responses and returns the original response without replaying the mutation. Reusing an ID with changed bytes gives `request_id_reused`. The cached response may describe an earlier phase: use `query_transaction` for current state.

After cache capacity is reached, reconnect using a **new session ID**, handshake and query the transaction. Never resend a state-changing command with a new ID just because its response was lost. If the current state proves it already ran, advance from that state. If it did not run, resubmit only the operation valid for the reconciled state. The cache is in-process, not durable, and is reset for a new session/core instance; the controller journal and state checks provide recovery across those boundaries.

## Minimal controller example

After verifying a `hello` from PID 4321 with creation time `134000000000000000`, send these JSON objects, each in its own length-prefixed pipe message. Replace the illustrative hash with the verified staged DLL's real SHA-256.

```json
{"major":1,"minor":0,"type":"welcome","client":"toolbox","session_id":"0123456789abcdef0123456789abcdef","request_id":"w1","pid":4321,"process_started":"134000000000000000","capabilities":["cooperative_update_v1","normal_lifecycle_v1"]}
```

Wait for `status`, stage/verify the update, and obtain the user's request before preparation:

```json
{"major":1,"minor":0,"type":"prepare_update","client":"toolbox","session_id":"0123456789abcdef0123456789abcdef","request_id":"p1","pid":4321,"process_started":"134000000000000000","transaction_id":"update-001","user_initiated":true,"artifacts":[{"name":"DBBox.dll","version":2,"abi":1,"sha256":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"}]}
```

Wait for `prepared` in all consumers:

```json
{"major":1,"minor":0,"type":"begin_unload","client":"toolbox","session_id":"0123456789abcdef0123456789abcdef","request_id":"u1","pid":4321,"process_started":"134000000000000000","transaction_id":"update-001"}
```

Wait for `released` everywhere, replace/verify the DLL, then:

```json
{"major":1,"minor":0,"type":"commit_update","client":"toolbox","session_id":"0123456789abcdef0123456789abcdef","request_id":"c1","pid":4321,"process_started":"134000000000000000","transaction_id":"update-001"}
```

Wait for `ready` from every participant, then send `finish_update` with a new request ID and the same transaction ID and wait for `ack`. This example is plugin-only. A full Toolbox update instead follows normal exit, independently verified file release, replacement and ordinary reinjection/startup; it does not send a core `commit_update` to start plugin loading.

## Existing launcher integration points

The inspected launcher is C++/Qt. `ArtifactUpdateService` already owns catalog synchronization, downloads and installation. Its current download/install path needs a stage-all barrier and participant coordination before it can replace live Toolbox artifacts. Catalog synchronization alone must never install files or unload anything.

`ToolboxPluginInventory` reads Windows FileVersion/ProductVersion and accepts a canonical decimal integer for plugin versions. DBBox now provides that string. Toolbox's `8.33.1` display is not a plugin integer: use the bridge's integer revision or the generated manifest. Map backend keys through the supported-artifact registry to canonical bridge names, artifact categories and the managed locations above. Display and manage only artifacts returned by the backend; neither a similarly named registered DLL nor an unsupported plugin becomes managed through discovery alone.

The game-process monitor/account model should supply participant discovery, PID/creation identity and the launcher-controlled launch/load barrier. Keep network I/O and pipe I/O off Qt's UI thread. Persist the file journal before every phase that can leave files or a process in a changed state; leave Toolbox's configuration and plugin state to its existing lifecycle logic.

A Toolbox core update is separate from the bootstrap-assisted `GWRLauncher.dll` self-update. Do not unload the launcher/controller during an active Toolbox file transaction; serialize those operations and resolve the Toolbox transaction before restarting the launcher.

## Acceptance testing

The standalone tests in `tests/gwrl` cover Windows message-pipe transport/framing, process identity, SHA-256, cancellation, ordinary reinjection correlation and rejection of legacy startup holds. A second suite compiles the actual GWRL module with a simulated Toolbox host and exercises capability negotiation, user initiation, selected-plan retention, unload/reload barriers, rollback, normal-exit dispatch, startup completion/failure, request replay and transaction completion. Both native suites pass. The host simulation does not replace the live-client checks below.

Required coverage includes:

- Two live clients sharing selected plugins but with different loaded sets: each reloads only what it unloaded, and previously unloaded plugins stay unloaded.
- Normal plugin settings save/load, and unchanged enabled selections, unselected plugins and up-to-date files.
- Full Toolbox exit through the existing save/shutdown path, followed by ordinary startup/autoload with `hold_plugins = 0` and no core commit or complete-inventory recovery command.
- A core-plus-plugin update, verification of the complete selected replacement set before any reinjection, and preservation of unrelated registered Toolbox copies.
- Backend-only inventory and first-time installation without automatically loading or enabling a feature.
- Missing/old bridges (including a peer without `normal_lifecycle_v1`), unreadable inventory, a busy plugin and an unexpected consumer of a target file.
- Disconnect after prepare/release/replacement, controller restart, request replay, hash mismatch, partial installation/reload/startup failure and safe binary rollback.
- Process exit/PID reuse, minimized Toolbox shutdown, distinct response `type` versus `state`, and notification without user approval causing no unload.

The Toolbox implementation and native command tests are included in this repository. Actual game shutdown/reinjection, plugin settings persistence and launcher integration still require the live-client acceptance checks above.

Use an intentionally failing **test** plugin to validate plugin-loaded crash dumps in a disposable test client, with its exact DLL/PDB pair. Do not deliberately crash a user's active game session as a build check.

Windows references: [named-pipe security](https://learn.microsoft.com/en-us/windows/win32/ipc/named-pipe-security-and-access-rights), [FreeLibrary reference semantics](https://learn.microsoft.com/en-us/windows/win32/api/libloaderapi/nf-libloaderapi-freelibrary), and [VERSIONINFO resources](https://learn.microsoft.com/en-us/windows/win32/menurc/versioninfo-resource).
