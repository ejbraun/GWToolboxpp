---
title: "DBBox Speedrun Scripting Tools"
description: "How DBBox runs triggered action sequences and recovers from interrupted map loads or failed actions."
section: features
---

Enable **SpeedrunScriptingTools** inside [DBBox](/docs/plugins/) to configure scripts made from conditions, triggers and actions. A script can launch after its trigger fires and its conditions pass. Scripts normally wait for other running scripts unless their parallel-launch option allows otherwise. A globally exclusive script holds back new scripts until it finishes or leaves its critical section.

Dialogue triggers match decoded text without Guild Wars formatting tags. Item, agent, skill and quest names supplied by the scripting helpers also use plain decoded text.

## Loading screens and action failures

Actions pause during loading screens. Map changes clear the old running actions and trigger state; instance-load scripts resume after the new map and player are ready. Fork revision 3 fixes a loading-state race that could leave scripting paused until another map change.

Some actions wait for game events. If an expected event never arrives, a script can otherwise keep later scripts waiting indefinitely. These failures now stop the affected script, release its active action state and identify the failed action in the script log:

- **Repop minipet** allows up to 30 seconds for the item cooldown and the expected spawn event.
- **Talk with NPC** allows up to 60 seconds for movement or a dialog, and stops when the target becomes invalid.
- **Keyboard movement** stops waiting after 5 seconds if movement never starts, or immediately if the movement function is unavailable.

Explicit **Wait** and **Wait Until** actions retain their configured behavior. A Wait Until condition that never becomes true can still intentionally hold a script. Check these conditions and critical sections when diagnosing a script that remains active.

Else-if branches now contribute to a conditional action's waiting behavior, so a blocking action in an else-if branch is allowed to complete. Loading a replacement script configuration clears the old active action state.

If a problem persists, include the DBBox version, the affected script or export, its trigger, the last reported action, and whether the issue began after changing maps. A report that scripts stop after roughly an hour does not by itself establish a time-based limit.
