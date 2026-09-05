---
title: "GWRL updates"
description: "User-requested launcher updates, in-game notifications, connection requirements and plugin crash support in this fork."
section: features
---

Open **Settings → GWRL** for launcher connection status and pending updates. A dismissible notification appears when the launcher reports a new available build. Repeated polling and transient empty reports do not repeatedly close/reopen it. GWRL is an unversioned module built into Toolbox and starts before plugin autoload. It ships with `GWToolboxdll.dll` and has no separate release version or manifest.

GWRLauncher manages downloads, verification, installation, rollback and reinjection. Toolbox's own release downloader is disabled. Updates start only when you select **Update now** here or explicitly request them in the launcher. Notification alone does not interrupt gameplay. The in-game status distinguishes a sent request from launcher acknowledgement and reports rejections, disconnects and unanswered requests. No acknowledgement or timeout triggers an unload by itself.

This fork implements the [GWRL communication protocol](/docs/gwrl_protocol/) for normal Toolbox restart and selective plugin reload. The launcher must implement the same contract and complete live-client integration testing before coordinated updates can be enabled.

## Connected clients and recovery

In-place update requires a compatible, actively connected bridge in every affected client. Older builds without a bridge and disconnected/unresponsive clients are not eligible.

For plugin-only updates, each Toolbox instance uses its existing save/unload logic for the selected outdated plugins it has loaded. The launcher waits for every affected instance and independently verifies file release, replaces only those outdated plugins, then permits reload. Each instance reloads only the selected plugins it had to unload; other plugins and saved enabled selections remain unchanged.

A full Toolbox update asks GWRL to invoke Toolbox's existing normal exit path, which owns saving configuration and shutting down plugins and Toolbox. The launcher independently confirms module absence, replaces the selected managed artifacts and reinjects Toolbox into the still-running game instances that unloaded it. Toolbox's normal startup restores its own settings and plugin selections. There is no startup hold or launcher-owned plugin-state snapshot.

A timeout or disconnect does not force unloading or permit replacement. During plugin-only updates, released plugins stay stopped until the launcher completes or rolls back the file transaction and tells the surviving Toolbox instance to reload. Full Toolbox recovery uses the same normal exit/startup paths. Closing a client is an available recovery option; the launcher preserves its file journal and blocks its own launches/injections from using partially updated files. It cannot control clients or injection tools started outside the launcher, so it must recheck file consumers before replacement.

## Versions and crash support

DBBox starts at integer revision **1**, including Windows FileVersion and ProductVersion text. Toolbox displays upstream plus a continuously increasing fork revision, initially **8.33.1**. Upstream's version definitions/export/comparisons remain unchanged; the fork revision never resets on an upstream bump.

This fork attempts to write crash dumps with plugins loaded and on older versions. Dumps include cached plugin names, known versions/hashes, active DBBox feature names, fork/build identifiers and the GWRL transaction. The crash handler does not invoke plugin callbacks.

Dumps remain local in `Documents/GWToolboxpp/<ComputerName>/crashes`. Share them manually with the fork maintainer alongside reproduction steps and the cached build metadata, which identifies the matching symbols kept locally by the maintainer. Dumps can contain process memory; use the maintainer's agreed private reporting channel. See [crash troubleshooting](/docs/troubleshooting/#crash-dump-errors) and [fork release metadata](/docs/fork_releases/).
