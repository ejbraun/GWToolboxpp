---
title: "DBBox Tactical Minimap"
description: "An independent DBBox minimap with modifier-based input, opacity, camera rotation and its own settings."
section: features
---

Enable **Settings → Plugins → DBBox → Tactical Minimap → Enabled**. It is disabled by default and uses the same start/stop lifecycle as the other DBBox features. **Visible** hides its window without unloading the feature.

The minimap draws walkable terrain, agents, the current target, your position and facing, aggro/spellcast ranges, and compass drawings/pings. Its window, zoom, rotation, opacity and settings (`Tactical Minimap.json`) are independent of Toolbox's built-in minimap and Mission Map. Either minimap can be enabled or hidden separately. This feature does not register Mission Map overlays.

## Controls

| Input | Default behavior |
| --- | --- |
| No modifier | Click through to the game |
| Alt + left click/drag | Ping or draw on the compass |
| Alt + wheel | Zoom this minimap |
| Alt + Shift + drag | Pan the view |
| Alt + Shift + double click | Recenter |
| Ctrl + left click/drag | Target the nearest living agent within 18 screen pixels |
| Both map and target modifiers | Targeting takes priority |

Map, target, pan and move-character modifiers are configurable. **None** disables the corresponding modifier action. Move-character clicks are disabled by default. Turn off **Enable tactical mode** to interact without holding the map modifier.

Unlock **Lock Position** or **Lock Size** to reposition or resize the window while the map modifier is held. With position unlocked, dragging the map body moves its window. **Recenter** and **Reset window position** are also available in settings.

Opacity fades the entire feature window, terrain, markers and drawings. At zero opacity it accepts no map input; settings remain accessible through DBBox. Neither the slider nor any other setting changes Toolbox's minimap or Mission Map.

Compass drawings/pings are normal Guild Wars party communication, so drawings sent here also appear in the normal compass and other viewers. That does not share this feature's display settings.

Disabling the feature saves its settings, detaches callbacks and drains queued work before releasing its instance. [GWRL updates](/docs/gwrl/) restore the saved DBBox feature selection.
