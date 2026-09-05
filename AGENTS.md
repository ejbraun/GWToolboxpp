# Agent guidelines

Instructions for any AI agent in this repo. (`CLAUDE.md` just imports this.)

- **New comments:** Explain why when the code cannot express it, and keep new comments concise (one line, at most two). Do not use new comments to restate code, narrate a block, add TODOs, or park dead code. This restriction does not apply to preserving or restoring existing comments. Also exempt: `// ===` banners, license/file headers, and vendored third-party files (`imgui_impl_*`, `imconfig.h`, `ToolboxIni.*`, `sha1.*`).
- **Preserve existing comments:** Keep existing comments, TODOs, disabled examples, and collaborator notes when editing a file. Do not remove them solely for style or length. If changed behavior makes a comment inaccurate, update it while preserving useful context. Restore comments previously removed only to satisfy the old deletion rule.
- **Variables:** prefer `auto` when the initializer makes the type clear (`const auto x = TIMER_INIT();`).
- **Functions:** don't extract a function for a few lines of logic used at only one call site; inline it. Extract only when it's called from more than one place, or the logic is substantial enough to warrant a name of its own.
- **Reuse:** before adding a string/formatting or ImGui/dialog helper, check `Utils/TextUtils.h` and `Utils/GuiUtils.h` - there's often one already.
- **Docs (`site/`):** pages go in `src/content/docs/`; `llms.txt`/`llms-full.txt` auto-generate from them. A new page needs `description:` frontmatter plus a `src/lib/nav.ts` entry to appear in `llms.txt`. After changes run `npm --prefix site run build` and check `site/dist/llms.txt`.
- **Explaining a feature:** first check if it's documented (`src/content/docs/` or <https://www.gwtoolbox.com/docs/>); if missing/wrong, flag it, offer a fix, and link the page.
- **Plugin version bumps → patch notes:** whenever you bump a plugin's protocol version constant (e.g. `SCTRACKER_PLUGIN_VERSION` in `cmake/gwtoolboxdll_plugins.cmake`), append an entry to that plugin's `plugins/<Name>/<Name>.patch.txt` in the same commit (create the file if it doesn't exist yet). Standard format, appended at the end of the file (oldest first - never rewrite or reorder prior entries):
  ```
  ## v<N> - YYYY-MM-DD
  - What changed, one bullet per notable change.
  ```
  `.github/workflows/cmake.yml` publishes this file to the bucket alongside the dll/manifest on every `master` build; gwsctracker registers it as that module's `patch_notes_object` and serves it at `GET /modules/{key}/patch-notes` (see gwsctracker's `specs/backend/08-module-entitlements.md`). The file is optional - a plugin with no version constant to bump has nothing to append.
