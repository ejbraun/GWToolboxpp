---
title: "Fork release versioning"
description: "DBBox and Toolbox integer revisions, patch notes, local checks and publishing workflow."
section: features
---

`cmake/fork_versions.cmake` defines independent `DBBOX_PLUGIN_VERSION` and `GWTOOLBOX_FORK_VERSION` integers, plus `GWTOOLBOX_PLUGIN_ABI`. Append chronological patch notes to `plugins/DBBox/DBBox.patch.txt` or `GWToolboxdll/GWToolboxdll.patch.txt` whenever its revision increases:

```text
## v2 - 2026-09-06
- Describe the release change.
```

Toolbox keeps upstream `GWTOOLBOXDLL_VERSION` and the numeric Windows version tuple. Its FileVersion/ProductVersion text and fork display become `<upstream>.<fork revision>`. The fork revision keeps increasing across upstream updates. SCTracker's sources, revision constant, generated header and notes are unchanged.

GWRL is an unversioned built-in Toolbox module. It ships inside `GWToolboxdll.dll` and has no separate CMake release version, manifest or published artifact. `GWTOOLBOX_PLUGIN_ABI` identifies binary compatibility between Toolbox and DBBox; `GWTOOLBOX_FORK_BUILD_ID` identifies the shared source build. The communication protocol version and capabilities identify wire compatibility independently of artifact release revisions.

## Local outputs and checks

The native build emits `DBBox.version.json` and `GWToolboxdll.version.json` beside the DLLs. Both contain `name`, integer `version`, UTC `compiled_at`, DLL `sha256`, `toolbox_abi` and `build_id`. The Toolbox manifest carries only its integer fork revision; the combined display version remains in the DLL. DBBox embeds its integer Windows version text and exposes `ToolboxArtifactInfo` to the running bridge.

DBBox requires a matching `GWTOOLBOX_PLUGIN_ABI`; a mismatch is refused before initialization. SCTracker retains its existing interface without requiring the new export.

Tests and release-metadata validation run locally only. The CMake workflows do not build, run or upload the test suites, and changes under `tests/` do not trigger them. Keep test outputs in the separate `build-gwrl-tests/` directory.

```powershell
cmake --build build --config RelWithDebInfo --target DBBox
py -3 scripts/validate-fork-release.py --bin bin/RelWithDebInfo
cmake -S tests/gwrl -B build-gwrl-tests -G "Visual Studio 18 2026" -A Win32
cmake --build build-gwrl-tests --config Release
ctest --test-dir build-gwrl-tests -C Release --output-on-failure
```

Keep matching DLL/PDB pairs locally, indexed by DLL hash. A revision alone cannot identify symbols if rebuilt. PDBs and compressed PDBs are excluded from Actions artifacts and release uploads.

## Publishing

`.github/workflows/cmake.yml` builds DBBox and Toolbox and publishes their generated manifests and patch notes. Changes to version header templates, patch notes and the manifest writer trigger the same builds as source changes.

| Output | Payload/destination |
| --- | --- |
| Actions build artifact | Build outputs, excluding `.pdb` and `.pdb.gz` files |
| DBBox metadata/notes | DLL, generated integer manifest and cumulative patch notes in existing `gs://${GCP_PLUGIN_BUCKET}/plugins/DBBox/` objects and GitHub release assets |
| Toolbox metadata/notes | DLL, generated integer manifest and cumulative `GWToolboxdll.patch.txt` in existing `gs://${GCP_PLUGIN_BUCKET}/plugins/GWToolboxdll/` objects and versioned GitHub release assets |
| Fork release tag/body | `gwtoolbox-v<upstream>.<fork revision>`, read from the DLL FileVersion resource, with cumulative Toolbox fork patch notes |
| Rolling plugin release | Plugin build outputs, excluding symbols, and available patch notes in the existing `plugins-latest` prerelease |

Publication remains restricted to master builds of the fork. Bucket uploads also require the existing Google Cloud secret and bucket variable. A versioned release is created only when its fork tag is absent; the rolling plugin release and configured bucket objects refresh on each eligible master build.

SCTracker keeps its existing revision, generated header, manifest and patch notes. The common upload filter excludes its PDB too. Symbols stay local; the workflow creates no build archive releases.

The launcher must verify hashes and retry a manifest/DLL mismatch: fixed bucket objects are separate writes, so publication cannot make concurrent downloads atomic.
