---
title: "DBBox Dhuum Calculator"
description: "Dhuum fight estimates in the Underworld, live progress updates and safe encounter resets."
section: features
---

Enable **Dhuum Calculator** inside [DBBox](/docs/plugins/). Its window estimates when damage and Dhuum's Rest will finish, and shows the remaining Furies or Rests when one requirement is complete.

The calculator operates in the playable Underworld instance. It identifies Dhuum by his NPC model and uses his current health, without requiring the old 80,000–81,000 maximum-health range. Outside the Underworld, the window stays hidden.

## Starting and resetting

Before Dhuum is available, the window displays **Dhuum not found**. If the feature is enabled partway through the fight, it displays **Waiting for Dhuum's Rest update** until a live mission-progress packet arrives. It does not recover progress by reading the old progress-bar memory layout.

**Calculating finish times...** means it needs enough changing health and Rest samples for an estimate. Estimates use recent samples and update at most four times per second; they depend on the game sending progress updates and do not assume a constant rate of future damage or Rest.

Map transfers, new instances, Dhuum despawning and feature unload clear encounter estimates. Drawing uses copied values instead of retaining an agent pointer. Packet callbacks are removed when the feature unloads.

If the feature still crashes, include the DBBox version, a dump if one was generated, and whether it happened on entering the Underworld, approaching Dhuum, starting the fight or filling the Rest bar.
