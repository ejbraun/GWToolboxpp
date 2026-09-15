---
title: "DBBox Speedrun Scripting Tools"
description: "How DBBox runs triggered action sequences and recovers from interrupted map loads or failed actions."
section: features
---

Enable **SpeedrunScriptingTools** inside [DBBox](/docs/plugins/) to configure scripts made from conditions, triggers and actions. A script can launch after its trigger fires and its conditions pass. Scripts normally wait for other running scripts unless their parallel-launch option allows otherwise. A globally exclusive script holds back new scripts until it finishes or leaves its critical section.

Dialogue triggers match decoded text without Guild Wars formatting tags. Item, agent, skill and quest names supplied by the scripting helpers also use plain decoded text.

## Use Skill actions

**Use skill** supports a SkillID or a skillbar slot. Both forms wait for Guild Wars to confirm that the matching character activated the requested skill. Instant skills also require their activation notification. Confirmation advances the script on the next update without an added ping delay; simply submitting a request or seeing the casting animation stop is not enough.

While confirmation is pending, SST checks the player and timeout once per measured ping, using the higher of the client's current and average ping. The interval is bounded between 50 ms and 5 seconds, with a 250 ms fallback when ping is unavailable. The timeout starts when the request is dispatched and allows twice the skill's normal activation-plus-aftercast time (up to 30 seconds), plus four ping intervals or one second, whichever is longer. A rising ping extends that allowance; a later decrease does not shorten it. These checks do not block the game thread.

An interrupted cast, rejected request, or missing confirmation stops the affected script with a log message and releases its queued actions. It does not advance to dependent actions as if the cast succeeded. Loading another script or changing maps cancels pending requests; missing skillbars and invalid slots fail safely. Explicit Wait and Wait Until actions remain available for other game states.

Use the updated Toolbox DLL together with DBBox: Toolbox's compatibility fixes refresh the game input frame and release simulated keys on the following game loop.

## Movement actions

For **Move to** and **Move to distance from current target**, the former **Immediately finish** option is now **Finish when movement starts**. Existing script exports use the updated behavior automatically. SST sends the move immediately and advances when the character starts moving or is already within the destination's accuracy range; it does not wait for the full journey.

If the character stays idle, SST retries the move at intervals based on the client's ping. Movement is checked every update, so success does not add a full ping of delay. The request times out after four ping intervals or one second, whichever is longer, measured from its first dispatch. Rising ping extends the allowance; falling ping only changes the retry interval. The same ping bounds and fallback described for skills apply.

A rejected request or timeout stops the affected script with a log message. Clearing or replacing the script cancels its pending movement-start requests and retries. The other movement modes keep their existing arrival and retry behavior.

## Loading screens and action failures

Actions pause during loading screens. Map changes clear the old running actions and trigger state; instance-load scripts resume after the new map and player are ready. Fork revision 3 fixes a loading-state race that could leave scripting paused until another map change.

Some actions wait for game events. If an expected event never arrives, a script can otherwise keep later scripts waiting indefinitely. These failures now stop the affected script, release its active action state and identify the failed action in the script log:

- **Repop minipet** allows up to 30 seconds for the item cooldown and the expected spawn event.
- **Talk with NPC** allows up to 60 seconds for movement or a dialog, and stops when the target becomes invalid.
- **Keyboard movement** stops waiting after 5 seconds if movement never starts, or immediately if the movement function is unavailable.

**Stop Script** intentionally ends the current run, including when used inside conditional or random actions. It skips the remaining actions without reporting an error, releases active action state, and leaves the script available for its next trigger.

Explicit **Wait** and **Wait Until** actions retain their configured behavior. A Wait Until condition that never becomes true can still intentionally hold a script. Check these conditions and critical sections when diagnosing a script that remains active.

Else-if branches now contribute to a conditional action's waiting behavior, so a blocking action in an else-if branch is allowed to complete. Loading a replacement script configuration clears the old active action state.

If a problem persists, include the DBBox version, the affected script or export, its trigger, the last reported action, and whether the issue began after changing maps. A report that scripts stop after roughly an hour does not by itself establish a time-based limit.
