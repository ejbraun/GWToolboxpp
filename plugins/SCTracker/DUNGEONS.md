# Implementation Spec — dungeon support in SCTracker (plugin)

Companion to the shared contract, which lives in the backend repo:
`uwtracker/specs/features/dungeons.md` (backend + frontend detail:
`uwtracker/specs/features/dungeons-impl-backend.md`).

**Status: implemented** on branch `feat/sctracker-dungeons` (plugin v16). SCTracker builds clean
(`build/` MSVC tree, Release Win32). Not yet runtime-tested in-game — see §10.

**Scope of this doc:** everything that changes in `plugins/SCTracker/` to make **every Eye of the
North dungeon GWToolboxdll's Objective Timer follows** (18 — see the shared spec §2.1) publish like
Domain of Anguish does. The backend and frontend need almost nothing (a seed row + a `MAPS`
entry); the plugin needs a genuinely new concept — a **run that spans 2–5 game instances**.

- **14 multi-level dungeons** drive the multi-instance lifecycle below (`kDungeonLevelToEntry`).
- **4 single-instance** — Ooze Pit, Secret Lair of the Snowmen, Fronis Irontoe's Lair (1 level),
  and Slavers' Exile (`Slavers_Exile_Level_5` only) — are just added to `kTrackedMapIds` and
  tracked exactly like DoA; no lifecycle code touches them.

Read `SCTracker.h`'s class comment and the FoW/DoA history in the shared spec first. Line numbers
below are pre-change references — treat them as "near here", not exact.

---

## 1. Why dungeons are different

Every area SCTracker tracks today — UW, FoW, DoA — is **one game instance**: one
`InstanceLoadInfo`, one `GameSrvTransfer` at the end, one party capture, one `PartyLog` entry,
correlated to one GWToolboxdll `ObjectiveSet` by `utc_start`.

A dungeon is **2–5 chained explorable instances** (`Catacombs_of_Kathandrax_Level_1` →
`_Level_2` → `_Level_3`), each its own `MapID`, each firing its own `InstanceLoadInfo` **and**
`GameSrvTransfer`. But GWToolboxdll's `ObjectiveTimerWindow` still models the whole dungeon as
**one `ObjectiveSet`**: `name` = the entry level's map name, `utc_start` stamped once at entry,
objectives named `"Level 1" … "Level N"`, the last one ending on `EventType::DungeonReward`
(`GWToolboxdll/Windows/ObjectiveTimerWindow.cpp`, `AddDungeonObjectiveSet`).

So the plugin must:

1. **Stamp `utc_start` once**, on the entry level, and publish under the **entry-level `map_id`**
   (so it correlates with GWToolboxdll's set — correlation is `utc_start ±2s` only — and matches
   the backend's `map_configs` row and `objective.name`).
2. **Keep the run alive across level transitions** — no re-capture, no re-stamp, no premature
   `PartyLog` write or vote.
3. **Finalize once**, when the dungeon is actually left (to an outpost / a non-dungeon map), with
   the outcome accumulated across all levels.

As-is, `OnInstanceLoadInfo` (`SCTracker.cpp:657`) re-stamps and `OnGameSrvTransfer` (`:1059`)
finalizes on *every* instance boundary, so a 3-level dungeon would write 3 truncated `PartyLog`
entries, none matching GWToolboxdll's single set, all dropped after `kObjectiveGiveUpTimeoutMs`.

---

## 2. Tracked-map registry (`SCTracker.cpp`, anon namespace)

`kTrackedMapIds` stays the **"start a run here"** set. It gains all 18 dungeon **entry** ids: the
Level 1 id for a multi-level dungeon, the single map id for a 1-level one, and
`Slavers_Exile_Level_5` for Slavers (GWToolboxdll only builds an ObjectiveSet for that final
level). Deeper dungeon levels are **not** here — they continue the run in progress rather than
starting a new one.

`kDungeonLevelToEntry` (anon-namespace `std::unordered_map<uint32_t,uint32_t>`) maps every level of
each **multi-level** dungeon (entry + deeper) to its entry id. Built by an `add({Level_1, …})`
lambda whose level lists mirror `ObjectiveTimerWindow.cpp`'s `AddDungeonObjectiveSet()` calls
verbatim — 14 dungeons, 2–5 levels each. `Catacombs_of_Kathandrax_Level_3` is non-contiguous with
`_Level_1/_2` in the GWCA enum; the `MapID::` name handles that.

Helpers:
- `IsDungeonLevelMap(map_id)` — is this any level (entry or deeper) of a multi-level dungeon.
- `DungeonEntryFor(map_id)` — the run key for such a level, or `0`.

(There is no `IsDungeonEntryMap` in the shipped code — `DungeonEntryFor(map_id) == map_id` is only
checked implicitly via the deferred-transfer comparison in `OnInstanceLoadInfo`.)

`kDungeonExitGraceMs = 30 * 1000` — the `ProcessDungeonRunLifecycle` backstop timeout (§5.3).

Adding a dungeon later = one `kTrackedMapIds` line (+ one `add({...})` line if multi-level) + the
backend changeset row. Nothing else changes.

---

## 3. New state (`SCTracker.h`)

Next to the FoW latch members (`fow_objectives_seen_done` / `fow_completed`, ~line 222):

```cpp
// --- Dungeon (multi-instance run) state ---
// A dungeon run spans 2-5 chained explorable instances (Level 1..N). pending_dungeon_entry is the
// entry-level map id of the in-progress dungeon (0 = the active run, if any, isn't a dungeon).
// A GameSrvTransfer inside a dungeon is deferred (dungeon_transfer_pending) rather than finalizing
// the run: OnInstanceLoadInfo then decides "next level" (keep the run) vs "dungeon over" (finalize).
uint32_t pending_dungeon_entry = 0;
bool dungeon_transfer_pending = false;
uint64_t dungeon_transfer_tick = 0;      // GetTickCount64() at the deferred transfer; backstop timer
uint32_t current_level_started_at = 0;   // time() of the current instance's load - death-grace anchor
                                          // (== pending_utc_start for a single-instance UW/FoW/DoA run)
bool dungeon_completed = false;           // latched by OnDungeonReward, FoW-style; see FinalizeRun
GW::HookEntry DungeonReward_HookEntry;
```

New private method declarations (near `OnObjectiveDone`, ~line 141):

```cpp
void OnDungeonReward();
void FinalizeRun();                    // extracted run-end body, called by OnGameSrvTransfer + the deferred path
void ProcessDungeonRunLifecycle();     // Update() tick: backstop-timeout finalize
void RebindPartyAgentsForNewLevel();   // re-map agent ids after a level transition (see 6.3)
```

Reset the dungeon members on every fresh run start (see §4).

---

## 4. `OnInstanceLoadInfo` — three cases (`SCTracker.cpp:657`)

Restructure so the **deferred-transfer resolution runs before the existing early-return**:

```cpp
void SCTracker::OnInstanceLoadInfo(const uint32_t map_id, const bool is_explorable)
{
    // (A) A dungeon run is waiting to see what the next instance is.
    if (dungeon_transfer_pending) {
        const bool same_dungeon = is_explorable && IsDungeonMap(map_id)
                                  && DungeonEntryFor(map_id) == pending_dungeon_entry;
        dungeon_transfer_pending = false;
        if (same_dungeon) {
            // Level transition: same run continues. No re-capture, no re-stamp of utc_start.
            current_level_started_at = static_cast<uint32_t>(time(nullptr));
            RebindPartyAgentsForNewLevel();     // agent ids are fresh in the new instance (6.3)
            return;
        }
        // Anything else (outpost, city, a different tracked area, an untracked explorable) means
        // the dungeon is over. Finalize with the outcome accumulated across every level, then fall
        // through - this same load might itself be a fresh tracked run (e.g. straight into UW).
        FinalizeRun();
    }

    if (!is_explorable || !kTrackedMapIds.contains(map_id)) {
        return; // not an area we start a run for
    }

    // (B) New run. Existing body, unchanged except the two dungeon lines noted.
    next_utc_start = static_cast<uint32_t>(time(nullptr));
    next_map_id = map_id;
    next_character_name.clear();
    if (const GW::CharContext* cc = GW::GetCharContext())
        next_character_name = PluginUtils::WStringToString(cc->player_name);
    restart_requested = true;

    run_active = true;
    wipe_detected = false;
    resigned_login_numbers.clear();
    dhuum_started = false;
    dhuum_completed = false;
    fow_objectives_seen_done.clear();
    fow_completed = false;
    tracked_item_id_to_model_id.clear();
    pending_role_skill_events.clear();

    // dungeon state
    pending_dungeon_entry = IsDungeonEntryMap(map_id) ? map_id : 0;
    dungeon_transfer_pending = false;
    dungeon_completed = false;
    current_level_started_at = next_utc_start;
}
```

**Case (C) — joined a dungeon already in progress** (loaded straight into `_Level_2+` with no
active run — a party member who joined mid-run, or a re-entry): optional for v1. To support it,
change the `kTrackedMapIds.contains(map_id)` gate in (B) to
`(kTrackedMapIds.contains(map_id) || IsDungeonMap(map_id))` and set
`next_map_id = IsDungeonMap(map_id) ? DungeonEntryFor(map_id) : map_id;` plus
`pending_dungeon_entry = DungeonEntryFor(map_id);`. The run then has no Level-1 timing, exactly the
"joined mid-run" degradation UW/FoW already tolerate — `ProcessSync`'s `IsRunCompleted` fallback
still classifies it. Recommend shipping it; it's ~3 lines and the alternative is silently losing
those runs.

> **District hops:** `OnInstanceLoadInfo` can re-fire for the *same* instance on a district hop
> (see the comment at `WriteLogEntry`). In a dungeon that can't happen (levels are explorable, no
> districts), so no extra guard is needed — but if a re-fire for the current entry map arrives
> while `run_active`, the `restart_requested` path already de-dups via `WriteLogEntry`'s
> `(utc_start, character_name)` erase.

---

## 5. `OnGameSrvTransfer` + `FinalizeRun` + the backstop

### 5.1 Extract `FinalizeRun()`

Move the body of `OnGameSrvTransfer` (`SCTracker.cpp:1064`–`:1124` — from `run_active = false;`
through the vote-open block) into a new private `FinalizeRun()`. It must own `run_active = false`
as its first statement (so both callers are trivial) and clear the dungeon members at the end:

```cpp
void SCTracker::FinalizeRun()
{
    run_active = false;
    pending_dungeon_entry = 0;
    dungeon_transfer_pending = false;

    if (restart_requested || active_capture || party_members.empty())
        return; // capture never completed; nothing worth logging

    // ... existing resign / wipe classification (unchanged) ...

    // Completion latch: add dungeon_completed alongside the existing two.
    if (end_reason != "wipe" && (dhuum_completed || fow_completed || dungeon_completed))
        end_reason = "completed";

    WriteLogEntry(pending_utc_start, pending_map_id, pending_character_name, end_reason, party_members);
    last_queue_scan_tick = 0;

    if (can_report_failures && !plugin_outdated) {
        if (end_reason == "wipe" || end_reason == "resign")
            OpenVote(PostRunVoteKind::Failure, pending_map_id, pending_utc_start, party_members);
        else if (end_reason == "completed")
            OpenVote(PostRunVoteKind::Mvp, pending_map_id, pending_utc_start, party_members);
    }
}
```

`pending_map_id` is set once in `CaptureParty` from `next_map_id` (`SCTracker.cpp:1153`) and is the
**entry-level** id for a dungeon — exactly what we publish and what `MapHasDhuumMechanics(pending_
map_id)` / `MapSizeHasRoles` key off. Nothing about `pending_map_id` changes on a level transition.

### 5.2 `OnGameSrvTransfer` becomes a dispatcher

```cpp
void SCTracker::OnGameSrvTransfer()
{
    if (!run_active)
        return;

    if (pending_dungeon_entry != 0) {
        // Defer: this may be an inter-level portal. OnInstanceLoadInfo resolves it.
        dungeon_transfer_pending = true;
        dungeon_transfer_tick = GetTickCount64();
        return;
    }

    FinalizeRun();
}
```

### 5.3 Backstop timeout in `Update()` (`SCTracker.cpp:1127`)

Add `ProcessDungeonRunLifecycle();` to `Update()`. It only handles the case where a dungeon
`GameSrvTransfer` was deferred and **no `InstanceLoadInfo` ever followed** (client crash / DC / the
player alt-F4'd on the load screen):

```cpp
void SCTracker::ProcessDungeonRunLifecycle()
{
    if (!dungeon_transfer_pending)
        return;
    // A real inter-level load screen is well under kDungeonExitGraceMs; past that with no new
    // instance at all, treat the run as over so its vote/log aren't stuck forever.
    if (GetTickCount64() - dungeon_transfer_tick > kDungeonExitGraceMs) {
        dungeon_transfer_pending = false;
        FinalizeRun();
    }
}
```

```cpp
constexpr uint64_t kDungeonExitGraceMs = 30 * 1000; // near kSyncScanIntervalMs family; §8.5
```

Primary signal is still the `InstanceLoadInfo` in §4 — this is only the "no event will ever come"
safety net.

---

## 6. Outcome tracking across levels

### 6.1 Wipe / resign — already map-agnostic, just need to survive the transfer

`OnPartyDefeated` (`wipe_detected`) and `OnWriteToChatLog`'s `kResignedPrefix` parsing
(`resigned_login_numbers`) are not reset on a level transition (§4 only resets them on a *new run*),
so a wipe or all-resign on Level 3 is still visible to `FinalizeRun`. Nothing to add.

### 6.2 Completion — `dungeon_completed` latch (recommended)

Register in `Initialize` next to `ObjectiveDone` (`SCTracker.cpp:586`):

```cpp
GW::StoC::RegisterPacketCallback<GW::Packet::StoC::DungeonReward>(
    &DungeonReward_HookEntry,
    [this](GW::HookStatus*, GW::Packet::StoC::DungeonReward*) { OnDungeonReward(); });
```

```cpp
void SCTracker::OnDungeonReward()
{
    if (run_active && pending_dungeon_entry != 0)
        dungeon_completed = true;
}
```

Remove the callback in `Terminate()` alongside the others (`:648`).

**If the latch is cut from v1:** a walked-out completed dungeon finalizes as `"unknown"`;
`ProcessSync`'s `IsRunCompleted` fallback (`:1473`) still upgrades it to `"completed"` and opens the
(late) MVP vote once GWToolboxdll's file lands — identical to how FoW behaved before plugin v15.
The latch just moves the vote prompt to the reward screen. Cheap; recommend shipping it.

### 6.3 Death tracking — re-bind agent ids per level

`agent_id_to_party_index` maps **in-world agent ids** → `party_members` index and is built once
during capture (`add_member`, `:1197`). Agent ids are **fresh in each new instance**, so after a
level transition the map holds stale Level-1 ids and `OnUpdateAgentState` silently stops counting
deaths. Re-bind on the transition:

```cpp
// Roster order (players, then heroes, then henchmen) is stable across a dungeon's levels, so we
// re-key agent_id_to_party_index by position. party_members / their accumulated .deaths are kept;
// only the agent-id lookup and the alive/dead edge state are rebuilt (everyone is alive on load).
void SCTracker::RebindPartyAgentsForNewLevel()
{
    agent_id_to_party_index.clear();
    party_member_currently_dead.assign(party_members.size(), false);
    const GW::PartyInfo* info = GW::PartyMgr::GetPartyInfo();
    if (!info)
        return;
    size_t i = 0;
    const auto bind = [&](uint32_t agent_id) { if (i < party_members.size()) agent_id_to_party_index[agent_id] = i; ++i; };
    for (const auto& p : info->players)
        if (const GW::Player* gp = GW::PlayerMgr::GetPlayerByID(p.login_number)) bind(gp->agent_id);
    for (const auto& h : info->heroes)   bind(h.agent_id);
    for (const auto& h : info->henchmen) bind(h.agent_id);
}
```

**Known limitation:** position-based re-keying drifts if a player leaves mid-dungeon (their slot
vanishes, heroes/henchmen after it shift). Deaths on later levels can then be misattributed.
Acceptable for v1 — dungeon SC is normally 8 humans and the failure mode is a wrong-participant
death count on a partial run, never a crash. **Robust follow-up:** add `uint32_t login_number` to
`PartyMember` (0 for AI) and re-key players by `login_number`, AI by `hero_id` / henchman order.

### 6.4 Death grace — per level, not per run

`OnUpdateAgentState` skips deaths while
`time(nullptr) - pending_utc_start < kDeathTrackingGraceSec` (`:822`). `pending_utc_start` is the
**entry-level** start, so on Level 3 the grace has long expired and the "everyone spawns, positions
settle" churn of that level's load is counted as deaths. Change the anchor to the per-level stamp:

```cpp
// was: pending_utc_start
if (static_cast<uint32_t>(time(nullptr)) - current_level_started_at < kDeathTrackingGraceSec)
    return;
```

`current_level_started_at == pending_utc_start` for a single-instance UW/FoW/DoA run, so their
behaviour is unchanged. `kDeathTrackingGraceSec` stays 60s; see §8.4 for the risk that a sub-60s
dungeon level (Frostmaw's L1–L4 can be very fast) then never counts a real death — consider
dropping it to ~20s, evaluated against real pacing.

---

## 7. Gate changes + what does NOT change

**`IsAcceptablePartySize`** — new `IsDungeonRun(map_id)` helper (any tracked map that isn't
UW/FoW/DoA); a dungeon id now accepts `real_player_count` `1..8`, same as FoW. `real_player_count`
is `CountRealPlayers` (`is_player` only), so a low-man run padded with heroes/henchmen still fails
the gate and isn't uploaded — the all-human roster count is the party size.

The rest is unchanged:

| Code | Why it's already correct for a dungeon |
|---|---|
| `MapSizeHasRoles` (`:220`) | Dungeon hits `default: return false;` at **every** size → `OpenVote` populates `pending_vote_names` from the roster → **name-mode vote** (credit/blame a character), same as DoA. Backend `FailureReportService`/`MvpReportService` accept `roles[]` as `raw_name`s for `role_model = NULL` runs. |
| `VoteRoleVisibleForMap` (`:247`) | Only consulted in role mode; irrelevant in name mode. |
| `MapHasDhuumMechanics` (`:237`) | UW-only. Every Dhuum latch (`OnAgentUpdateAllegiance`, `OnObjectiveDone`), the gambling-stone chat parser, the post-Dhuum death cutoff and the Ecto-drop suppression all early-return on a dungeon. |
| `kTrackedItems` / item-drop tracking | No dungeon entries in v1 (Frostmaw's greens etc. are a later "dungeon item boards" feature). |
| `ProcessSync` / `TryReadMatchingObjectiveEntry` / `IsRunCompleted` | Correlation is `utc_start ±2s`; the plugin stamps once at entry, GWToolboxdll stamps its dungeon `ObjectiveSet.utc_start` once at entry → they match. `IsRunCompleted` (`objectives.back().status == Completed`) is true once GWToolboxdll marks the last `"Level N"` objective done on `DungeonReward`. |
| `CaptureParty` | Runs once, on the entry level (`restart_requested`), before the first `GameSrvTransfer`. Level transitions don't set `restart_requested`, so it isn't re-run. |
| `CanTerminate` | No new `AsyncRestClient`. |
| Publish payload shape (`PublishPayload` / `LogEntry`) | `map_id` = entry-level id, `party_members` = 8 players, `objective` = GWToolboxdll's `"Level 1".."Level N"` set. No new fields. |

---

## 8. Risks / decisions

1. **Does GWToolboxdll flush an `ObjectiveSet` for an abandoned dungeon?** `StopObjectives()` (which
   marks unfinished objectives `Failed`) runs only when the *next* `ObjectiveSet` is added, and
   returning to an outpost adds none. **Verify in `ObjectiveTimerWindow`** what triggers the
   `ObjectiveTimerRuns_*.json` write (`runs_dirty` / `SaveRuns`). If an incomplete dungeon never
   reaches disk, the plugin's `PartyLog` entry times out of the sync queue
   (`kObjectiveGiveUpTimeoutMs`, 10 min) and is dropped with `CancelPendingVoteIfMatching` — a
   quiet, acceptable loss. Document it; no code needed.
2. **Case (C) joined-mid-run** — ship it (§4) or drop those runs? Recommend ship (~3 lines).
3. **`dungeon_completed` latch** — ship in v16 (recommended) or lean on `IsRunCompleted` fallback?
4. **Per-level death grace duration** — keep 60s (misses real deaths on fast Frostmaw's levels) or
   drop to ~20s? Decide against real run pacing.
5. **`kDungeonExitGraceMs`** — 30s proposed. The `InstanceLoadInfo` in §4 is the real signal; this
   only fires when no load event ever comes (crash/DC). Long enough to never pre-empt a slow load,
   short enough that a crashed run's vote/log isn't stuck the whole session.
6. **`RebindPartyAgentsForNewLevel` position drift** if a player leaves mid-dungeon (§6.3) —
   accept for v1, note the `login_number` follow-up.
7. **Map ids** — 560 / 570 / 630 computed from the GWCA enum; the plugin uses the `MapID::` names
   so a wrong *number* here only matters for the backend seed. Still worth a live `party.map_id`
   capture to confirm before the backend changeset lands.

---

## 9. Versioning & docs — **done**

- `cmake/gwtoolboxdll_plugins.cmake` — `SCTRACKER_PLUGIN_VERSION` `15` → `16` (generated header +
  `SCTracker.version.json` confirm `version: 16`).
- `plugins/SCTracker/SCTracker.patch.txt` — v15 (backfilled) + v16 entries appended. v16 reads:
  all 18 dungeons at party size 1–8 (all-human), role-less, multi-level = one run under the
  entry-level id, `DungeonReward` completion latch.
- `plugins/SCTracker/README.md` — "What it does": add the dungeons and a one-line multi-instance
  note ("a dungeon run spans its Level 1..N instances and is tracked as one run").
- `site/src/content/docs/plugins.mdx` (GWToolboxpp `site/`) — update if it enumerates tracked areas
  (and run `npm --prefix site run build`, per `AGENTS.md`).
- **Do not** ask the backend team to raise the enforced minimum plugin version — older clients
  simply never tracked dungeons.

---

## 10. Manual QA (no integration-test harness for this path)

Run each with a machine key pointed at a test backend that has changeset `055` applied.

| Scenario | Expected |
|---|---|
| Full clear of a 3-level dungeon, exit via the reward → outpost | one `PartyLog` entry, `map_id` = entry id, `end_reason = "completed"` (latch), one `/upload-run` → `200 created:true`, MVP (name-mode) vote popup at the reward screen |
| Full clear, but leave by everyone resigning after the reward | `end_reason` still `"completed"` (latch beats resign), MVP vote |
| Wipe on Level 2, booted to outpost | `end_reason = "wipe"`, deaths counted for L1 **and** L2 (after per-level grace), failure (name-mode) vote |
| Resign on Level 3 without finishing | `end_reason = "resign"`, `completed = false` server-side, failure vote |
| Enter Level 1, immediately map out | capture incomplete → `FinalizeRun` early-returns, nothing logged/uploaded |
| 3-human party (no AI) | accepted — `party_size = 3`, role-less, name-mode vote |
| 3-human + 5-hero party | dropped client-side (`CountRealPlayers` = 3 ≠ roster 8), no upload |
| 9+ real players (impossible, sanity) | dropped by `IsAcceptablePartySize` |
| Alt-F4 on the Level 2 load screen | after `kDungeonExitGraceMs` the backstop finalizes; entry syncs or times out normally |
| Second party member also running the plugin, same run | backend dedups on `(entry map_id, entry utc_start, roster)` → one run, participants merged |
| Verify `utc_start` correlation | plugin's `PartyLog_*.json` `utc_start` within ±2s of the matching `ObjectiveTimerRuns_*.json` `ObjectiveSet.utc_start` for the same dungeon |
