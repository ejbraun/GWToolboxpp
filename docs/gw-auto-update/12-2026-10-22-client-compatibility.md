# October 22, 2026 client compatibility

Client **39108** crashes Toolbox 8.33.8 during GWCA initialization because the
September compatibility layer is enabled only for client 38974. The new client
still needs that layer. Its packet table has 488 entries, while unpatched GWCA
expects 487.

The v9 change removes the client fingerprint gate and enables the existing
compatibility implementation whenever the pinned bundled GWCA is loaded. Guild
Wars updates its client automatically, so executable identity is not used to
choose an older layout or refuse an unfamiliar build. A harmless game update
will no longer skip required compatibility or disable Toolbox solely because
its hash changed. The GWCA hash and instruction/operand validation remain:
these patches target fixed addresses inside that specific DLL.

The investigation also found four added skill records, a larger quest limit,
changed map text, another language-file column, internal store message changes,
and a movement implementation change. These findings are separate from the
startup assertion; their implications are recorded below.

## Inputs and crash evidence

| Input | Previous | Latest |
|---|---|---|
| Native build | 38974 (`0x983e`) | 39108 (`0x98c4`) |
| PE timestamp | 1790796237 | 1792686324 |
| Image size | `0xf4c000` | `0xf49000` |
| PDB GUID and age | `107FCEE4F00540D5A290144E8817BCF82D` | `107FCEE4F00540D5A290144E8817BCF82E` |
| SHA-256 | `8e50edfb83515fabfec50bfd9d17968e8635a39b4c3e796e311bbc5cd15e5878` | `7363584ba5159366fee7f5eb68053c53e155da373b300cd8c6af49429647979c` |

Inputs were copied from `C:/Projects/GWRLauncher/dependencies/Gw_previous.exe`
and `Gw_latest.exe` into the dated research directory before analysis. The
previous input exactly matches the client analyzed in the September report, so
its existing Ghidra corpus was reused. The latest client was imported into a new,
separate Ghidra 12 project. The native build getters are at previous VA
`0x4729e0` and latest VA `0x472a10`.

The new dump is
`8.33.8-20261022-121154-46840-46356.dmp`, from the user's crash directory.
Its client timestamp, image size and PDB identity match the latest input.
The dump contains Toolbox build `91f37ec4b664` and bundled GWCA 4.8.5.0.
This is a separate incident from the October 20 loading crash.

Evidence for the initialization failure:

- Exception `0xe06d7363` and the release assertion text `'' in '' line 2`.
- Stack slot `0x088cfb40` contains GWCA `+0x14367`, the return address after
  the handler-count assertion. This is a stack candidate corroborated by the
  assertion and native array, not a symbolized complete stack walk.
- The native array at `0x02436654` contains buffer `0x04a6eb40`, capacity
  504, size **488**, growth 21.
- The bundled DLL's count operand at RVA `0x14315` is **487**. The existing
  compatibility layer changes it to 488, but the old production client guard
  skips that layer for the latest executable.

Bundled GWCA SHA-256 remains
`b33375229bcb06e415b71246ba317966c0ec6c76f71f284bc49f1783685e1975`.
The neighboring `C:/Projects/GWCA` source checkout is not an exact source match
for this DLL. Actual DLL disassembly and scanner replay were used for its
private implementation details.

## Binary and scanner coverage

| Check | Result |
|---|---|
| Ghidra recovered functions | 18,463 previous; 18,444 latest |
| Latest decompilation | 18,091 completed; 125 failed; 228 thunks excluded |
| Relocation/branch-normalized function matches | 18,058 previous functions; 15,649 have a unique latest destination |
| Remaining previous functions | 405 do not match the strict fingerprint; 127 still lack a match after additionally masking 32-bit integer constants |
| Bundled GWCA scanner replay | 225/227 resolved on both clients |
| Active Toolbox scanner replay | 88/90 resolved on both clients |
| Enclosing scan functions | 165 pairs; nine have normalized instruction changes |
| Additional plugin probes | 11 resolved on both; one deliberately absent duplicate probe on both |
| Native send-function callers | All 173 recovered latest callers have a normalized counterpart in the previous client |

These fingerprints preserve field displacements and ordinary immediates in the
strict comparison, while removing relocations and external branch destinations.
They are evidence about instruction bodies, not proof of every transitive callee
or every runtime object. Masking integer constants was a second navigation pass;
it is not counted as unchanged behavior.

The same four base scanner failures occur before and after the update: GWCA's
`FRAME_STATE_CREATED` assertion probe and dependent function lookup, and the
Toolbox crash-handler string probe and dependent lookup. One runtime-dependent
GWCA scanner call remains excluded. Four resolved base results lie outside
Ghidra's recovered function bodies, including three data addresses.

A separate source-pattern survey also has identical outcomes: 97 unique,
23 ambiguous, eight absent and five skipped raw anchors on each input. Its
absent anchors include stale external GWCA source and Toolbox `Unused` files;
they are not newly broken active-DLL scans.

Of the nine changed scan-owner pairs, five only change assertion line numbers.
The others change the native build constant, skill count, language-row stride,
and internal store message values.

## Packets, messages and structure layouts

**No additional StoC header renumbering or recovered payload-schema change was
found.** Both clients have 452 recovered non-null schemas in the same header
positions. The full native runtime table still contains 488 slots. The September
legacy-to-native translation at `0x194` remains necessary.

All 452 descriptor-associated handler bodies match after normalization:
422 use Ghidra function fingerprints; the remaining 30 small handlers were
compared separately through their terminating instruction and following padding.
The latter comparison preserves internal branches and field offsets. There are
447 distinct handler addresses in those descriptors.

For public `0x100000xx` UI messages, 456 distinct IDs align across 2,102 paired
instruction occurrences with **no shifts or conflicts**. The existing September
translation is retained. This comparison does not establish the semantics of
every unpaired message or callback payload.

The checked character and item access paths retain these native layouts:

| Field or record | Latest value | Evidence |
|---|---|---|
| Game-context character pointer | `+0x44` | Native load handlers and email getter |
| Character load flags | `+0x228` | Native packet `0x194`, handler `0x84d590` |
| District / language | `+0x22c` / `+0x230` | InstanceLoadInfo handler `0x84d780` |
| Observed / current map | `+0x234` / `+0x238` | Same handler |
| Observed / current map type | `+0x23c` / `+0x240` | Same handler |
| Player flags / player number | `+0x2ac` / `+0x2b0` | Same handler |
| Account email | `+0x3d0` | Native getter/setter `0x84bed0` / `0x84c060` |
| Skill record | `0xa4` bytes | Native getter and every table record |
| AttributeInfo record | `0x14` bytes | 51 constant records compared |
| AreaInfo record | `0x7c` bytes | Native getter `0x5a66e0` |
| Item formula | `0x18` bytes | Native getter `0x5a72a0` and all records |
| Material cost | `0x10` bytes | Every referenced formula material list |

InstanceLoadInfo remains 32 bytes, including the September-added trailing flag.
The old 28-byte emulation remains rejected. No compatibility operand, packet
mapping or public UI translation needed a new value for this update.

The outgoing send routine and its 173 recovered callers also retain normalized
instruction bodies. In particular, ChestOpener's native equivalents still send
header `0x51` with 12 bytes and `0x53` with eight bytes, at latest VAs
`0x91f720` and `0x91f7c0`. This is static coverage of recovered send paths, not a
capture of every server interaction.

## Skills, quests, maps and other constant data

### Skills and attributes

The skill count increases from **3,495 to 3,499**. The table moves from VA
`0x98b4b0` to `0x988478`; every record's ID equals its index in both inputs.

For all existing IDs 0–3494, every byte is unchanged except the no-PvP-variant
sentinel at offset `0x2c`: 3,102 records change that value from 3495 to 3499.
Actual PvP mappings, name/description IDs, professions, attributes, skill types,
flags, costs and timings retain their values. There is no existing-skill
renumbering in this table.

| Added ID | Type | Name string ID | Description | Concise description |
|---|---:|---:|---:|---:|
| 3495 | 16 (`Skill2`) | 101503 | 101504 | 101505 |
| 3496 | 16 | 101506 | 101507 | 101508 |
| 3497 | 16 | 101509 | 101510 | 101511 |
| 3498 | 16 | 101512 | 101513 | 101514 |

Localized names were not decoded from the asset archive, so no guessed enum
names were added. Toolbox's skill enumeration/bounds callers use the bundled
DLL's scanned `GetSkillCount()`, which already handles the expanded table.
Existing `skill_id_pvp` consumers do not compare against the old numeric sentinel.
The 51 AttributeInfo records are byte-identical, including profession/attribute
IDs and name/description references.

### Maps and quest limit

Both inputs contain **898 AreaInfo records** at the same 124-byte stride.
Map IDs, campaigns, continents, regions and region types retain their positions
and values. The data differences are:

- In 881 records, `needed_pq` changes from `0x5b9` (1465) to `0x5c5` (1477).
  Native quest validation and the no-prerequisite comparison change by the same
  amount. This supports an expanded quest-ID bound/sentinel; it does not prove
  the names or semantics of twelve added quests.
- Map **886** changes name string ID `0x13bad` to `0x18a77` and description ID
  `0x13bae` to `0x18a78`.
- All other map-record fields are unchanged, including the remaining actual
  prerequisite references. Active Toolbox code does not hardcode the old
  `needed_pq` sentinel.

There are pre-existing catalogue gaps: the bundled `MapID::Count` and GetMapInfo
bound stop at 883, and the named SkillID enum already ends before the previous
client's full skill table. Those gaps were not introduced by this update. They
should be handled as a separate catalogue/API expansion with verified names,
rather than renumbering existing constants.

### Formulas, language data and internal store messages

All **1,509 item formulas** have unchanged fields and referenced material lists
after resolving their relocated pointers. The 24-byte stride patch remains
correct. No material-slot adjustment was found in these records.

The language-file row grows from **100 to 101 pointers**, still with 18 language
rows. The getter multiply changes from `0x190` to `0x194`. InfoWindow already
derives the row length and obtains each row through the native getter, so only
its explanatory comment needed updating.

The internal store/UI code shifts observed values `0x20000006`–`0x2000000d` up
by one at affected dispatch/registration sites. These are outside the public UI
range translated by Toolbox. Other subsystems use some of the same numeric
values unchanged, including map-resource chunk IDs: **a global numeric remap
would be incorrect**. The shipped GWCA decompilation and active source callers
contain no corresponding hardcoded store-message constants requiring a patch.
Store/control implementation changes account for a substantial part of the
remaining unmatched code; their full new behavior was not reconstructed.

## Plugin-specific result and live follow-up

Additional replay covers ChestOpener's sender/server pointer, Dialogs' complete
raw-send lookup chain, Base pathfinding, Scripting ping and keyboard movement,
and SkinChanger's item notification including its uniqueness check. All resolve
as before; the duplicate notification probe is correctly absent on both clients.
Eight of nine distinct enclosing functions have unchanged normalized bodies.

The exception is Scripting's native **SideWalk** routine, moving from
`0x535b50` to `0x533350`. Its six-argument calling convention and scan anchor
remain intact. The new routine removes the old clear of movement-state bit
`0x4` before updating the destination. Toolbox does not apply a fixed-offset
patch there, so no binary repair is indicated. The gameplay effect of that
native change remains unverified; test SST forward, backward, left and right
keyboard movement, completion and cancellation in a live client.

Live testing should also cover injection/loading, hotkeys, travel, salvage
tooltips, inventory actions, skin refresh, dialogs, chat and unload/reinjection.
No game process was launched or injected during this analysis. Static agreement
and isolated fixtures do not establish compatibility of every runtime struct,
unrecovered function, new server behavior or external plugin.

## Implementation and validation

Production changes are in `GWToolboxdll/GWToolbox.cpp` and the InfoWindow comment:

1. Remove the `Gw.exe` timestamp, image-size and file-hash gate.
2. Activate the existing compatibility layer for the pinned bundled GWCA,
   independently of the client build. There is no separate legacy-client path
   or refusal for unfamiliar client fingerprints.
3. Retain GWCA identity and instruction/operand checks. Non-pinned GWCA DLLs
   retain their existing handling. Actual changes to game layouts or protocols
   still require analysis; removing the gate does not detect such changes.

The local Win32 fixture now extracts the actual production hot-patch functions
and supplies executable headers and file paths without executing client code.
It loads the real bundled GWCA DLL and checks the resulting handler-count
operand, formula stride and packet translation. It also varies client metadata
and paths to verify that executable identity cannot skip activation.

Validation completed in both configurations:

- Release and Debug: all three CTest cases pass, covering activation with the
  previous and latest client fixtures plus the full GWCA compatibility fixture.
- Activation checks include changed client timestamps/image sizes, different
  client file hashes, missing/truncated client paths, repeat application,
  restoration and non-pinned GWCA handling.
- The existing fixture covers all 487 legacy packet IDs, all 465 legacy/468
  native UI IDs, six character getters, current-player identity with conflicting
  flags, all 1,509 formulas, hook rollback and lifecycle.
- Toolbox `GWToolboxdll` builds successfully in Debug and RelWithDebInfo.

The branch remains v9, with existing 8.33.8 version metadata. No release version,
plugin protocol, bundled GWCA binary or client executable was changed. No commit
was made. Regression fixture files remain under the repository's existing ignored
`tools/gw-update/tests/` directory; the analysis and binaries are local artifacts.

## Evidence and candidate

Local evidence is under `research/client-update-2026-10-22/`:

- `crash-identity.json`, input snapshots, Ghidra project/logs and decompilation.
- Both scanner replay CSVs, `all-scan-owner-changes.diff`, plugin replay and
  `plugin-comparison.json`.
- `comparison.json`, `packet-handler-supplement.json`, `ui-message-map.json`,
  `outgoing-callers.json`, and integer-constant comparisons.
- Skill/attribute and map/formula comparisons, including the complete raw added
  skill records.
- `tests-Release.log`, `tests-Debug.log`, and the two Toolbox build logs.
- `package/RelWithDebInfo/` contains the candidate DLL and compressed matching
  PDB; `package/manifest.json` records identities and SHA-256 hashes.

The working builds are `bin/RelWithDebInfo/GWToolboxdll.dll` and
`bin/Debug/GWToolboxdll.dll`. See the local fixture
[README](../../tools/gw-update/tests/README.md) for reproduction commands and
the [September report](11-2026-09-30-client-compatibility.md) for the retained
packet/UI/character/formula translations.
