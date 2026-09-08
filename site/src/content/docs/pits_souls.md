---
title: "DBBox Pits and Souls"
description: "Chained Soul death and respawn estimates for Bottom, Double and Reaper in the Underworld."
section: features
---

Enable **Pits and Souls** inside [DBBox](/docs/plugins/). The window tracks the Chained Souls near the **Bottom**, **Double** and **Reaper** locations in the Underworld and estimates when they will die or respawn.

Souls are identified by the decoded English name **Chained Soul**, with formatting tags removed. Identification works with other client languages and does not depend on a fixed model ID. Name decoding can take a moment; pending results from an earlier map or an unloaded feature are discarded.

Move within compass range of a soul to begin tracking. Once a health sample or death event has been received, estimates continue after leaving range. The feature retains its existing timing model: 103.6 seconds of life from full health and 120 seconds between death and respawn. These are estimates; new health values and death events correct the timing.

## When the window says Unknown

- **Unknown (soul not found):** no Chained Soul has been acquired near that location. Approach the location with the feature enabled.
- **Unknown (waiting for health):** the soul has been identified, but the client has not supplied usable health or a death event. A corpse first seen after its death does not reveal when it died.
- **Waiting for the Underworld:** tracking starts in a playable Underworld instance.

The four friendly Pits quest spirits are different NPCs from these Chained Souls and are not used to start the timers.

## DBBox revision 5

Acquisition accepts the nearest Chained Soul within 500 game units of each location, replacing the old 50-unit requirement. It can use map-agent health when the visible agent has no usable health value, and listens for soul spawn and death events.

An unchanged cached health value no longer restarts the death estimate each frame. Countdown timing continues across missed frames and leaving compass range. Map changes and feature unload clear the old encounter state.

If a row remains Unknown while standing beside its Chained Soul, include the exact row text, location, DBBox version and whether the soul is currently alive in the report.
