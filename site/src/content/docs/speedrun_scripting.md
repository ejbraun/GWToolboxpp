---
title: "DBBox Speedrun Scripting Tools"
description: "How DBBox runs triggered action sequences and recovers from interrupted map loads or failed actions."
section: features
---

Enable **SpeedrunScriptingTools** inside [DBBox](/docs/plugins/) to configure scripts made from conditions, triggers and actions. A script can launch after its trigger fires and its conditions pass. Scripts normally wait for other running scripts unless their parallel-launch option allows otherwise. A globally exclusive script holds back new scripts until it finishes or leaves its critical section.

Dialogue triggers match decoded text without Guild Wars formatting tags. Item, agent, skill and quest names supplied by the scripting helpers also use plain decoded text.

## Use Skill actions

**Use skill** supports a SkillID or a skillbar slot. Both forms wait for the game's cast-completion notification before advancing to the next action. Instant skills also need this confirmation. This keeps a sequence such as Target NPC → Use Skill → Talk with NPC → Send Dialog from treating a failed cast as successful.

Rejected or interrupted casts stop the affected script and appear in its log. A cast that never starts or finishes times out after 5–30 seconds, depending on its normal casting time. Loading another script or changing maps cancels any pending skill request. Missing skillbars and invalid slots fail safely.

Use the updated Toolbox DLL together with DBBox: Toolbox's compatibility fixes refresh the game input frame and release simulated keys on the following game loop.

## Loading screens and action failures

Actions pause during loading screens. Map changes clear the old running actions and trigger state; instance-load scripts resume after the new map and player are ready. Fork revision 3 fixes a loading-state race that could leave scripting paused until another map change.

Some actions wait for game events. If an expected event never arrives, a script can otherwise keep later scripts waiting indefinitely. These failures now stop the affected script, release its active action state and identify the failed action in the script log:

- **Repop minipet** allows up to 30 seconds for the item cooldown and the expected spawn event.
- **Talk with NPC** allows up to 60 seconds for movement or a dialog, and stops when the target becomes invalid.
- **Keyboard movement** stops waiting after 5 seconds if movement never starts, or immediately if the movement function is unavailable.

Explicit **Wait** and **Wait Until** actions retain their configured behavior. A Wait Until condition that never becomes true can still intentionally hold a script. Check these conditions and critical sections when diagnosing a script that remains active.

Else-if branches now contribute to a conditional action's waiting behavior, so a blocking action in an else-if branch is allowed to complete. Loading a replacement script configuration clears the old active action state.

If a problem persists, include the DBBox version, the affected script or export, its trigger, the last reported action, and whether the issue began after changing maps. A report that scripts stop after roughly an hour does not by itself establish a time-based limit.
