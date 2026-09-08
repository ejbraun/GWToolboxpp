---
title: "Objective Timer"
description: "Track run objectives, collapse active runs and keep history across sessions and multiple Toolbox instances."
section: windows
---

Open **Objectives** from the Main Window or with `/show objectives`. Entering DoA, FoW, UW, the Deep, Urgoz, supported dungeons or tracked tournament maps starts the corresponding objective set.

Click a run's header to expand or collapse its objectives. Collapsing an active run keeps it running. Use the header's close button to remove it from the current window.

## Run history

**Settings → Objectives → Save/Load runs to disk** keeps records between sessions. Runs are saved during map transitions and through normal Toolbox settings saves and shutdown. The most recent 200 saved records are loaded on startup. **Show past runs** includes records from previous days; today's records are shown without that option.

History lives in `Documents/GWToolboxpp/<ComputerName>/runs/ObjectiveTimerRuns_YYYY-MM-DD.json`. Dates in filenames use UTC; the window displays local dates and times. History is separate from the selected configuration folder.

Fork revision 3 fixes collapsed runs disappearing when the window is away from the left edge of the screen. It also merges each instance's runs into existing history instead of overwriting earlier sessions. Separate runs that start in the same second remain distinct.

Saving uses background snapshots, coordinates access between instances and completes the file replacement before normal Toolbox unload finishes. A failed save leaves the previous file intact and reports an error. Malformed history files are preserved for recovery rather than overwritten.

All clients sharing a history folder should use a build with these fixes. Older builds can still overwrite the shared daily file. Runs already overwritten by an earlier build can only be recovered from an existing backup.

## Display options

The Objectives settings include start, end and duration columns, extra time precision, run start dates, detailed DoA objectives and a separate current-run window. **Automatic /age on completion** sends the age command when the run finishes.
