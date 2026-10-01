# September 30, 2026 client compatibility

The new client breaks the bundled GWCA 4.8.5.0 through changed packet IDs, UI
message IDs, character-context offsets and an item-formula stride. Toolbox v8
contains a temporary compatibility layer for this exact client/GWCA pair. It
patches the loaded GWCA module before `GW::Initialize`; neither executable nor
the bundled GWCA DLL is modified on disk.

Debug and RelWithDebInfo Toolbox builds and the actual-GWCA regression fixture
pass. The intermediate compatibility build reached a map before the second dump
exposed the item-formula problem. The user subsequently reported that the v8
candidate worked except for hotkey keybinds. The follow-up fixes below still need
live validation; full gameplay and unload coverage remain incomplete.

## Binary identity and analysis coverage

| Input | Identity |
|---|---|
| New client | `C:/Projects/GWRLauncher/dependencies/Gw_latest.exe` |
| New client SHA-256 | `8e50edfb83515fabfec50bfd9d17968e8635a39b4c3e796e311bbc5cd15e5878` |
| Previous client SHA-256 | `44fbd68767a8d02b5dd4fb1a8a09b684a86b24716731327ee64905dd698fe124` |
| Bundled GWCA SHA-256 | `b33375229bcb06e415b71246ba317966c0ec6c76f71f284bc49f1783685e1975` |
| New client PE timestamp / image size | `1790796237` / `0xf4c000` |
| New client PDB GUID and age | `107FCEE4F00540D5A290144E8817BCF82D` |
| Native GW build | `38974` (`38888` previously) |

The client identity matches both crash dumps. The native build is confirmed by
the constant returned at client VA `0x4729e0` and by the second dump's game report.
The separately reported file ID `390284` is not the native build number.

Ghidra 12.0 performed full automatic analysis in separate local projects for
both clients and the shipped GWCA DLL. The shared Ghidra project was not edited.

| Corpus | Functions | Selected for decompilation | Completed | Failed |
|---|---:|---:|---:|---:|
| Previous client | 18,438 | 18,210 | 18,087 | 123 |
| New client | 18,463 | 18,235 | 18,112 | 123 |
| GWCA, including recovered module initializers | 1,760 | 1,760 | 1,760 | 0 |

Client exports exclude 228 thunks each. Normalizing relocations and external
branch targets matched 17,607 old functions, including 15,203 with a unique new
destination; 831 did not match. This is a navigation aid, not proof of semantic
equivalence or coverage of every runtime object layout.

Scanner replay used the bundled DLL's actual scanner implementations against
mapped client images, without executing client code:

| Calls replayed | Previous | New |
|---|---:|---:|
| GWCA | 225 resolved / 227 | 225 resolved / 227 |
| Current Toolbox, excluding `Unused` | 88 resolved / 90 | 88 resolved / 90 |

The same four failures exist on both clients: GWCA's repeated
`FRAME_STATE_CREATED` assertion search and dependent function lookup, and
Toolbox's crash-handler string search and dependent lookup. One GWCA scan whose
arguments depend on a runtime stack value was excluded. No new lookup failures
were found among the replayed calls.

Successful lookup alone is insufficient: GWCA still found the item-formula table
but indexed it incorrectly. Comparing the enclosing functions of all resolved
scans covered 164 distinct function pairs; 31 had normalized instruction changes.
Those changes include the formula and language-table strides, ID shifts, table
counts, assertion line numbers and native implementation changes. Four scan
results were outside Ghidra's recovered function bodies, including three data
addresses. The packet descriptor and UI-message comparisons below provide
additional coverage beyond those scan targets.

## Loading crash: packet table and headers

Dump `8.33.7-20260930-211104-18208-47176.dmp` fails in GWCA's StoC initializer:

- GWCA return address `+0x14367` follows the handler-count assertion.
- The compare at `+0x14314` expects `0x1e7` (487).
- The dumped native handler array has 488 entries and capacity 504.
- The release assertion counter is 2; missing file/message text is expected for
  that GWCA release build.

The new client inserted native header `0x194`. Every legacy header from `0x194`
through `0x1e6` moves up by one. For example, legacy InstanceLoadFile `0x195`
becomes `0x196`, and InstanceLoadInfo `0x199` becomes `0x19a`. The added handler at
client VA `0x84fc00` updates the new character-context flags at `+0x228` and emits
UI message `0x10000113`.

Static descriptor harvesting recovered 451 old and 452 new non-null schemas.
After the header remap, the only payload-schema difference in those descriptors
is InstanceLoadInfo: it gains a DWORD at packet offset `0x1c`, making it 32 bytes
instead of 28. The legacy prefix remains intact; the new flag affects player
flags. Legacy InstanceLoadInfo **emulation is rejected** because a caller's old
28-byte object does not contain the new field. Incoming native packets remain
available to existing callbacks through their unchanged prefix.

The compatibility layer patches the handler-count operand and translates GWCA
registration, removal and emulation calls. The registry retains native indexes.
Callbacks temporarily see their registered legacy header; the native header is
restored before client handling and on callback return. Internal registration's
call to removal is guarded against translating twice. The inserted packet is
left to its native handler.

PacketLoggerWindow also indexes the client's native schema table directly. Its
callback receives a legacy header, so using that header as a native table index
selected the preceding schema after the insertion. The logger now uses
`GWCACompatibility::NativePacketHeader` for schema lookup and hover details, and
`LegacyPacketCount` for its registration and selection bounds. Saved filters and
displayed packet IDs remain in the same legacy numbering as GWCA callbacks. The
new native-only packet is excluded from the legacy selection list.

## UI-message IDs

The native UI sequence inserted `0x10000113`, `0x10000148` and `0x10000181`.
The legacy-to-native mapping is:

| Legacy range | Adjustment |
|---|---:|
| Below `0x10000113` | 0 |
| `0x10000113`–`0x10000146` | +1 |
| `0x10000147`–`0x1000017e` | +2 |
| `0x1000017f`–`0x100001d0` | +3 |

Other ranges, including GWCA's `0x300000xx` pseudo messages, remain unchanged.
The comparison recovered 404 distinct message mappings from 1,615 paired
instruction occurrences with no conflicting mappings. Boundary functions were
also checked directly. Examples include preference flags `0x10000141` →
`0x10000142`, travel `0x10000183` → `0x10000186`, and append-to-chat
`0x10000195` → `0x10000198`.

GWCA's UI and frame registration, removal and outgoing-send APIs translate
legacy callers to native IDs. Wrapped callbacks receive legacy IDs. Native
dispatch wrappers enter the original send implementations directly, so incoming
native IDs are not translated again. This also preserves nested sends from
callbacks. ChatCommands' directly hooked native chat callback uses
`GWCACompatibility::NativeUIMessage` for its comparison. Minimap's native compass
callback now uses the same helper for quest-added and active-quest comparisons,
and for the quest-removed replacement message used to hide quest markers. These
quest IDs shift by two; its low-valued frame messages are unchanged.

## Character context

The insertion at `CharContext+0x228` shifts fields used by bundled GWCA getters
and direct Toolbox accesses:

| Field | Old offset | New offset |
|---|---:|---:|
| District | `0x228` | `0x22c` |
| Language | `0x22c` | `0x230` |
| Observed map | `0x230` | `0x234` |
| Current map | `0x234` | `0x238` |
| Observed map type | `0x238` | `0x23c` |
| Current map type | `0x23c` | `0x240` |
| Player flags | `0x2a8` | `0x2ac` |
| Player number | `0x2ac` | `0x2b0` |
| Timer fields | `0x370`, `0x374` | `0x378`, `0x37c` |
| Native account email | `0x3c8` | `0x3d0` |

The bundled header's email offset `0x3c0` was already stale relative to the old
native getter. The new offset is established by the native getter/setter at
`0x84e540` / `0x84e6d0`, rather than by adding four to the header value.

Seven displacement operands repair six GWCA getters: GetMapID, GetLanguage,
GetIsObserving, GetDistrict, GetInstanceType and GetPlayerNumber. Toolbox's direct
player-flag writes and account-email read use compatibility accessors. Hotkeys
and reroll use the corrected GWCA player lookup, as detailed below. Other active
direct reads found in the follow-up audit use unchanged fields before `+0x228`,
including character name, world ID and server address. The vendor's public
struct is not rewritten.

### Follow-up: hotkey keybinds

After the crash fixes, the user reported that clicking a hotkey's Run button
worked but pressing its keybind did not. `CheckSetValidHotkeys` still passed
`CharContext::player_number` directly to `GetPlayerByID`. The old offset `0x2ac`
now holds player flags; the correct player number is at `0x2b0`. The explicit
wrong ID bypassed the patched GetPlayerNumber getter, selecting a different or
nonexistent player and preventing the active-hotkey list from being populated.
Run calls `Toggle` directly and bypasses that list.

The hotkey check now uses `GW::PlayerMgr::GetPlayerByID()` with its documented
default current-player lookup. Reroll's party-leader selection had the same stale
read; it now calls `GW::PlayerMgr::GetPlayerNumber()` instead of its local struct
access helper. The fixture reproduces both an in-range wrong player with no
profession and an out-of-range flags value, and verifies that the real GWCA
default lookup returns the correct player in both cases.

## Follow-up crash: item-formula stride

Dump `8.33.7-20260930-221733-48928-70236.dmp` contains the intermediate
RelWithDebInfo candidate, PDB GUID/age
`D55AD4B41A474E0F9D793370DD19F9B550`. Its matching DLL/PDB were preserved before
the final rebuild. Symbolization identifies:

1. `AppendSalvageInfo`, ItemTooltipModule.cpp:148, at Toolbox RVA `0x13d1c7`.
2. The item-tooltip description callback.
3. ItemDescriptionHandler's native description hook.

The client had reached map 857 for 11 seconds. The fault is a read at `0x1`,
not the earlier StoC assertion. The item has formula index `0xe3` (227).
GWCA computes `table + 227 * 20`, landing in the middle of a new record; the
misread material-buffer field is exactly `0x1`.

The old native getter at `0x5a95b0` uses stride `0x14` and count `0x5dd` (1,501).
The new getter at `0x5a99f0` uses stride `0x18` and count `0x5e5` (1,509). The
first 20 bytes retain their layout, and material-cost entries still occupy 16
bytes. For formula 227, both clients correctly identify material IDs 6 and 17
when indexed with their respective strides.

Changing the single-byte multiply immediate at GWCA RVA `0xd868` from `0x14` to
`0x18` repairs GetItemFormula for all consumers without replacing its bounds
checks or public return type. The fixture tests every valid new index, the
out-of-range index and null input, including reading a material through the
returned pointer.

## Other changes and patch lifecycle

The native language-file table changed from 18 rows of 99 pointers to 18 rows of
100. InfoWindow's export now obtains rows through the scanned native getter and
derives the row length, avoiding the old hardcoded stride. A pre-existing GWRL
template warning also needed an `else` to make the Debug `/WX` build pass.

The follow-up source audit covered active Toolbox code and bundled plugins,
excluding `GWToolboxdll/Unused`. It checked direct character-context accesses,
native UI callbacks, native packet-table indexing, packet emulation and formula
consumers:

| Call sites | Result |
|---|---|
| Hotkeys and reroll player identity | Replaced stale struct reads with corrected GWCA getters. |
| Native chat and compass callbacks | Compare or emit native IDs through compatibility. Other inspected native callbacks use unchanged low frame IDs. |
| Packet logger | Translates legacy headers when indexing native schemas and limits selection to legacy packet IDs. |
| GameSettings, InfoWindow and ToolboxUtils | Changed flags/email fields use compatibility accessors. Remaining character-context reads use unchanged name, world-ID or host fields. |
| ItemTooltipModule and InventoryManager | Obtain formulas through the corrected GWCA getter; neither indexes the native formula table directly. |
| ArmorSwap, TrackerAdvanced and SCTracker | Direct character-context reads only use the unchanged name field. ArmorSwap's equip message passes through translated SendUIMessage. |
| Slowload and other packet-emulation callers | Use translated GWCA APIs. Slowload replays InstanceLoadFile, whose payload is unchanged; no active caller emulates the enlarged InstanceLoadInfo. |

GWCA-registered UI/packet callbacks continue using legacy constants because the
layer translates their inputs and outputs. Native hooks and table reads require
the explicit helpers above; translating a normal GWCA caller first would apply
the adjustment twice.

The entry point is `ApplyGWCAHotPatches` in GWToolbox.cpp, with implementation in
`Utils/GWCACompatibility.cpp`. Existing targeting and key-input patches remain.
Activation requires the pinned GWCA SHA-256, matching client PE metadata and the
exact client SHA-256. A fingerprint failure for matching PE metadata refuses
initialization. Other binary pairs do not activate this compatibility layer.

All nine operand preimages are validated before mutation. Initialization refuses
an already populated packet registry. Ten hooks are installed before normal
GWCA initialization, with rollback on creation or enable failure. Unload removes
only hooks created by the layer and restores its operands. No vendor sources,
vendor binaries, protocol ABI constants or client files are edited.

## Validation and remaining scope

`tools/gw-update/tests` loads the exact bundled GWCA DLL into an isolated Win32
test process. It exercises its real registries, exports and private codec
initializer with synthetic game data; it does not inject into Guild Wars.

Both Release and Debug pass:

- All 487 legacy packet IDs, native delivery, inserted-packet isolation,
  native-table lookup indexes and legacy counts, callback order,
  replacement/removal, blocking and supported emulation.
- All 465 legacy UI IDs in both global and frame APIs, incoming and outgoing;
  all 468 native IDs; inserted IDs; nested sends; blocking and pseudo messages.
- Six character getters, current-player lookup despite conflicting character
  flags, direct player flags and email, and all 1,509 formulas.
- Operand mismatch rejection, partial-hook rollback, idempotent initialization,
  restoration and refusal of an already initialized registry.

Both Toolbox configurations build as `8.33.8`; release metadata matches fork
revision 8. The v8 notes also include the branch's existing SST retry/cancellation
changes; DBBox remains at its existing revision 10. No commits were made.

These results cover the confirmed failures and tested API translations. They do
not establish that every payload, game subsystem or third-party plugin is
compatible. A plugin that reads old struct fields directly or hooks native UI
callbacks outside GWCA must handle the native layout/IDs itself. New protocol
features are not exposed through the old headers. Live validation should include
injection before and after loading, hotkey keybinds, reroll party-leader selection,
hidden compass quest markers, packet-content logging, salvage tooltips/name tags,
travel, chat, inventory actions, representative SST scripts and unload/reinjection.

## Local evidence

The ignored `research/client-update-2026-09-30/` directory contains:

- `ghidra/ClientUpdate.gpr`, `PreviousClient.gpr`, `GwcaUpdate.gpr` and headless
  analysis logs.
- `latest-decompiled.c`, `previous-decompiled.c`, `gwca-decompiled.c`, inventories,
  function ranges, calls, strings and symbols.
- `comparison.json`, packet descriptors, normalized fingerprints,
  `ui-message-map.json`, and the scanner replay sources/results.
- `all-scan-owner-changes.json` and `.diff`, extending the initial hook-target
  comparison to enclosing functions of every resolved scan.
- `direct-character-review.txt`, `direct-ui-review.txt` and
  `compatibility-caller-review.txt`, recording the follow-up caller audit.
- `crash-triage.*`, `crash-221733.*`, and the matching intermediate DLL/PDB in
  `crash-221733-binaries/`.
- `tests-Release.log`, `tests-Debug.log`, Toolbox build logs and final candidate
  copies under `package/` with SHA-256 hashes.

See the fixture [README](../../tools/gw-update/tests/README.md) for repeatable
build/test commands. The compatibility layer should be removed or re-derived
when the upstream GWCA update is adopted; its private RVAs are specific to the
pinned DLL.
