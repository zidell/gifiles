# Gifiles

Finder-style cross-platform file manager (macOS · Windows · Linux) built with Qt 6 Widgets, C++20,
with an embedded terminal that is wired to the file list. User-facing text is written in Korean
(the source language) and translated to English, Japanese and Simplified Chinese (see i18n below).
README.md describes features and shortcuts for users; this file is for whoever changes the code.
What has been built so far, why things are the way they are, measurements and open items:
`docs/history.md` — read it before starting new work.

Git: public repo `github.com/zidell/gifiles` (MIT, `LICENSE`), branch `main`. GitHub Actions
(`.github/workflows/build.yml`) builds and runs all tests on Windows (MSVC), Linux (Ubuntu 22.04)
and macOS (universal) on every push; **every push to main that passes becomes release
`v0.1.<run number>`** (Windows zip, Linux AppImage, signed and notarized macOS zip once its secrets
exist, signed `update.json`) and the installed apps update to it (see Automatic updates below). So a
push to main ships to users: push only what passes locally. Pushes touching only `*.md` / `docs/`
don't build or release. The history is public now: new commits, no more rewriting. Check CI after
pushing (`gh run watch`); the Windows test logs are the `smoke-windows` artifact. Issues,
Discussions and Wiki are off: the README says requests aren't taken (fork under MIT).

## What the user wants (product principles)

- **"My own Finder"**: macOS Finder is the reference for behavior, naming and shortcuts (⌘1 gallery,
  ⌘2 list, ⌘3 columns, Space Quick Look, Return rename, ⌘↓ open, …). When unsure how something
  should behave, do what Finder does — unless a rule below says otherwise.
- **Cross-platform, native, light**: Qt Widgets, no web view (the user rejected ~100 MB idle web
  runtimes). Keep idle memory low; load heavy parts (media player, PDF, terminal shell) lazily.
- **Keyboard first**: every feature must be reachable from the keyboard.
- **The terminal link is the core strength** ("터미널과의 상호 호환이 강점"): list ↔ terminal must
  feel like one tool.
- **Speed over effects**: no fades or animations (Quick Look closes at once; the terminal folds at
  once). The user asked for this explicitly, twice.
- **Look**: modern flat design, dark-mode aware, subtle (stripes, dimmed secondary columns). The
  user dislikes a "공대스러운" (engineer-made) look.

## How to work with this user

- Answer in Korean, briefly. The user writes quick, informal Korean with typos; read for intent.
- **Developer-machine workflow is in `local/`** (gitignored, not in the repo): if `local/AGENTS.md`
  exists, read it and follow it (rebuild/relaunch after every change, signing).
- Messages often arrive mid-task, several at once; handle all of them, in one pass where possible.
- When a visual choice is genuinely ambiguous (e.g. "where should the color go?"), ask once with
  AskUserQuestion and ASCII previews; otherwise decide and say why in one line.
- Verify UI on real rendering with `GIFILES_SNAPSHOT` (below) and crop/inspect the PNG, not only
  offscreen tests. **Don't drive the user's live desktop** (System Events keystrokes, clicks):
  the Gifiles window is usually behind other windows and input lands in other apps.
- Report what was measured vs. only reasoned; say plainly what wasn't verified on a real screen
  (e.g. two-monitor behavior, real mouse drags).

## Workflow (every change)

1. Build and run the tests: `cmake --build build && ctest --test-dir build --output-on-failure`
   (configure once: `cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_PREFIX_PATH=$(brew --prefix qt)`;
   one suite: `./build/gifiles_<suite> [test names]`).
   **Every test is headless and must stay so** (the user works while they run): each suite includes
   `tests/Headless.h` first, which puts it on Qt's offscreen platform unless `QT_QPA_PLATFORM` is set;
   never launch the app or a real window from a test, never use `open` without `-g`.
   Put each test where it needs the least: logic without widgets (TOML, settings, quoting, helpers,
   RecentFolders, the CLI options — it runs the built app) in `tests/unit_core.cpp`; file operations
   and their undo in `tests/unit_fileops.cpp`; updates in `tests/unit_update.cpp`; one widget on its
   own (preview, terminal, sidebar, settings window, dialogs) in `tests/unit_widgets.cpp`; only what
   needs the whole MainWindow (keys across panes, tabs, the list ↔ terminal link) in `tests/smoke.cpp`.
   Add or extend a test for each behavior change. Coverage of all suites together:
   `scripts/coverage.sh` (instrumented `build-cov`; `scripts/coverage.sh show src/X.cpp` for lines).
2. Release build in `build-rel` (see README); on a developer machine, `local/` (above).
3. For UI changes, check real rendering (native icons, dark mode):
   `GIFILES_SNAPSHOT=<dir> build-rel/Gifiles.app/Contents/MacOS/Gifiles <folder>` saves
   list/gallery/column screenshots of the first window (first row selected in list) and quits.
   On macOS it runs in the background (`macStayInBackground`: never activated, the user's focus
   stays), so windows render as inactive; `GIFILES_SNAPSHOT_FOREGROUND=1` when active colors matter.
   Put scratch output in the session scratchpad, not in the repo. `sips -c H W --cropOffset Y X`
   crops; a tiny Swift script with `NSBitmapImageRep.colorAt` can sample pixel colors.

Dependencies (macOS): `brew install qt cmake ninja libvterm pkgconf`.

## Rules that are easy to break

- **Signing:** sign the macOS Release app with a fixed identity (`GIFILES_SIGN_IDENTITY`, bundle id
  `dev.zidell.gifiles`). macOS ties Full Disk Access to the signature; the linker's ad-hoc signature
  changes every build and makes macOS ask for folder access again.
- **Shortcut keys:** in Qt, "Ctrl" is ⌘ on macOS and "Meta" is the Control key. Give every shortcut
  explicitly per platform; don't rely on `QKeySequence::StandardKey` (offscreen tests use generic
  bindings, and a sequence registered twice is treated as ambiguous and silently ignored —
  the `shortcutsAreUnique` test guards this). The Korean input source turns \` into ₩, so the
  terminal toggle accepts both ⌃\` and ⌃₩ (and the terminal lets both through).
- **Deleting:** the app never deletes permanently. Removal goes to the trash (`QFile::moveToTrash`);
  "replace" in a name conflict trashes the old item. Every file operation records an `UndoRecord`
  (FileOps) so ⌘Z / ⇧⌘Z work; new operations must do the same (add a `Step` kind if needed, with
  both directions in `replay`).
- **Settings live in `config.toml`** (`Settings`, policy:
  github.com/zidell/agent-configuration-accessibility): every preference, the shortcuts, sidebar
  favorites and "항상 이 앱으로 열기" apps. Each item is in the schema in `Settings.cpp` (`items()`)
  with its description, type, range/choices and default, and the app writes the whole file with
  those comments on every change; a file edited outside the app is re-read at once (watcher), a
  wrong value is reported (`App::showConfigProblems`) and the old value kept. A new preference
  goes into `items()` — never straight into `QSettings`, which only keeps state (session,
  window geometry). `packaging/readme.txt` (shipped in the bundle / next to the .exe / in the
  AppImage) and `--help`/`--config-path`/`--print-config`/`--print-default-config`/`--check-config` (`main.cpp`,
  before the GUI starts) must stay in step with it. Tests use `GIFILES_CONFIG_DIR`.
- **Shortcuts:** the built-in keys of every menu action are listed once in `Shortcuts.cpp`
  (`entries()`, menu order, per platform); `MainWindow::act(title, fn)` registers the action by its
  menu title. The user's keys are `[shortcuts]` in config.toml (Settings → 단축키, "재지정"
  opens `KeyCaptureDialog`: it captures every key, Return/Esc included, so only its buttons close it;
  a key another action holds is named in small red text but can still be assigned — it moves over,
  since a key on two actions works for neither; the page's 초기화 / 전부 제거 act on all shortcuts and confirm first). App-defined keys outside actions also live in the same registry with a UI context (files,
  preview, terminal, sidebar, search, path, rename, menu). Conflict detection respects those contexts.
  Defaults follow macOS, with ⌘ represented by Ctrl on Windows/Linux, on every platform (Windows
  too: Return renames, Ctrl+Down opens — no Windows-only keys). A ` key also answers to ₩.
- **"선택한 항목들로…" commands:** config.toml `[selection_menu] commands`, each `{ id?, label, key, terminal,
  command }` (per-platform defaults in `Settings.cpp`: `new_folder` and `zip` quiet, `ai` in the terminal). The
  built-in ones (with `id`) can be edited but not removed (`withBuiltins` puts a missing one back; the page's
  삭제 is disabled for them). `MainWindow::runSelectionCommand` → `runCommandNow`: `terminal = true` →
  `runInTerminal` (same never-clobber rule), `false` → `runQuietly` (QProcess: login shell `-l -c`;
  PowerShell `-EncodedCommand` with `$ErrorActionPreference = 'Stop'`; only failures are reported; what it
  created in the folder gets selected — the new folder, the archive).
  Placeholders are filled by `TerminalWidget::expandCommand`, every value quoted by `TerminalWidget::quoteWord`
  ($'...' with control characters escaped; PowerShell "..." with backticks, typographic quotes escaped too):
  never paste a path into a command any other way. `{prompt}` asks with `MainWindow::askLine` (a one-line box centered on the window).
  The ⌃⌘N built-in stays (undoable). The old `[ai]` table and the old AI button's shortcut are gone; `[ai]` is ignored without a warning.
- **Context-menu letters:** `showContextMenu` gives every item a configurable key shown as "(O)" by default (Windows
  conventions, listed in README) and `MenuLetters` runs it on that key (a Korean 2-set jamo counts as its
  key; a submenu opens with its first item chosen). The shared actions' texts are restored when the menu
  hides. User commands use their own `key`.
- **Front window and ⌘W:** on macOS the menu bar sends keys to the main window's actions while Settings,
  Get Info or Quick Look is in front; the close actions close that front window first (`closeFrontWindow`).
- **i18n:** every user-facing string is `Gifiles::tr("한국어")` (context `Gifiles`, `Util.h`); never a
  bare `QStringLiteral` with Korean. Menu actions keep their Korean title as id (`act(id)`,
  `Shortcuts::entries()`, config.toml `[shortcuts]` keys) and show `Gifiles::tr(id)`; tables of ids
  use `QT_TRANSLATE_NOOP("Gifiles", ...)`. After changing text run `scripts/i18n.sh` (lupdate +
  lrelease), fill the new entries in `i18n/gifiles_{en,ja,zh_CN}.ts` and commit the .ts and .qm
  (builds embed the .qm and don't need Linguist; `translationsLoad` fails on unfinished ones).
  Language: config.toml `general.language` (system → ko/ja/zh/en by the system languages,
  English otherwise), applied at start (`main.cpp installTranslations`). Tests run untranslated.
- **Debug log** (`Log`): while the app runs from this checkout (the build machine), `main.cpp`
  writes `logs/gifiles-YYYY-MM-DD.log` (gitignored, 7 days kept): keys, actions with the state
  before/after, navigation, jobs and errors, dialogs, config reloads, Qt warnings. Typing in text
  fields and the terminal is only counted. Use it to analyse what the user reports.
- **Copy/paste:** both conventions work together: Mac (⌘C, then ⌘V copies / ⌥⌘V moves) and
  Windows (Ctrl+X then Ctrl+V moves). Cut items are dimmed until pasted.
- **Terminal ↔ list:** the terminal cd's with the browser only when the shell is idle and the
  command line is empty (otherwise it waits, or opens a new terminal tab — see Terminal tabs); items dragged onto the terminal are typed in as
  quoted paths (selecting alone never touches the command line, and the drop is always a copy
  action so the list never moves the files); the shell's own cd moves the browser. Never send
  input that could clobber a half-typed command (`m_typed`, `Pty::isShellIdle`).
- **Terminal tabs:** the panel's bar holds Chrome-like tabs (`m_termTabs` + `m_termStack`, same
  order). The browser's folder changes go to the current tab (`MainWindow::terminalFollow`): idle →
  cd; a program running in it (`TerminalWidget::isBusy`, after the shell's first prompt) → a new
  tab opens at the folder; only a half-typed line → wait (never a new tab: the drag-paths workflow
  types a command, then browses). Tabs opened for the browser never take the keyboard. Switching
  tabs moves the browser to that shell's folder; only the current tab's own cd moves the browser.
  ⌘T/⌘W/⌃Tab act on terminal tabs while the terminal has focus (Ctrl+T/W inside the
  terminal on Windows/Linux, matching the Mac defaults; users can reassign them). `Pty` closes the master before
  waiting for the shell (waiting first hung on a shell with a running program).
- **Terminal panel:** its header bar always stays at the bottom; folding hides the terminal and
  gives the space to the list (`setTerminalHeight(kTermHeaderHeight)`); dragging the folded edge
  up unfolds at that height, dragging an open one down to the bar folds it. No animation.
- **Terminal text:** libvterm marks the right half of double-width characters with 0xFFFFFFFF —
  skip those cells; macOS file names are NFD, so compose cell text to NFC before drawing.
- **AI:** there is no toolbar button any more (removed at the user's request); AI is the selection
  menu's built-in `ai` command. `{prompt}` is asked in a small one-line box centered on the window
  (`askLine`).
- **Automatic updates** (`Updater`): only builds configured with `GIFILES_UPDATES=ON` (CI's
  packages; `main.cpp` calls `Updater::allowUpdates()`) check `releases/latest/download/update.json`.
  `update.json.sig` must verify (Ed25519, TweetNaCl in `third_party/tweetnacl`) against `kPublicKey`
  in `Updater.cpp`; the private key is the repo secret `UPDATE_SIGNING_KEY` (copies: `local/update-signing-key.pem` on
  the developer's Mac and a backup the user keeps outside it; never in git) — the release job refuses a key that doesn't match
  `kPublicKey`, and changing the key strands every installed copy. The asset's size and SHA-256 must
  match; on macOS the unpacked app must also satisfy the codesign requirement (identifier
  `dev.zidell.gifiles`, team AF68GKBM82). A verified download waits in the cache; the toolbar shows
  업데이트, and on quit (or that button) a helper script (`Updater::writeHelper`: sh / PowerShell)
  waits for the process to exit and swaps the app in (copy beside, rename old away, rename new in,
  restore the old on failure). Off: builds by hand, Linux without `$APPIMAGE`, a folder the app
  can't write, `general.auto_update = false` (automatic checks only). Tests never update (they don't
  call `allowUpdates`); `tests/unit_update.cpp` runs the whole flow against file:// releases.
  macOS packages need the secrets (same names as the user's other apps) `APPLE_CERTIFICATE`
  (Developer ID .p12, base64; copy in `local/developer-id.p12`), `APPLE_CERTIFICATE_PASSWORD`,
  `APPLE_ID` + `APPLE_PASSWORD` (app-specific password, for notarytool), `APPLE_TEAM_ID`; without them
  no macOS asset is published and Macs simply see no update.
- **System preview:** what Qt can't draw goes to the OS (`SystemPreview`): office documents first
  (`isOfficeDocument`, before the text view), then anything that would end as an info card. macOS embeds
  `QLPreviewView` in a window container (a key monitor takes the keyboard back from it so Space/Esc/arrows
  still reach Qt); Windows hosts the extension's `IPreviewHandler` in a native child (not tried by hand);
  Linux converts office files to PDF with LibreOffice (`convertWithOffice`, cache, own profile). Offscreen
  there is none. Qt's `grab()` can't see the native view: `GIFILES_SNAPSHOT_QUICKLOOK=<file>` (with
  `GIFILES_SNAPSHOT`) saves `quick-look.png` through `screencapture`.
- **Folder tree** (\`, `FolderTree`): the index is built off the GUI thread and cached
  (`CacheLocation/folder-tree-<hash of the options>.bin`); it is in memory only while a panel is open (+2 min).
  Only `main.cpp` calls `prefetch()`, so tests never scan the drive (they set `folder_tree.roots`). Network, FUSE
  and virtual mounts are never crossed unless listed as a root (one NFS mount took 5 minutes). On macOS without
  Full Disk Access, Desktop/Documents/Downloads and other apps' containers are scanned only after the browser opened
  them (`noteVisit`), so a background scan never makes macOS ask. The keys inside the panel are fixed
  (`FolderTreePanel::eventFilter`, listed at its bottom) and not in the shortcut registry, at the user's request;
  only the ` that opens it is a menu action. **Indexing is automatic; ⌘R is only the fallback** (the user's
  rule): on macOS the cache stores the FSEvents journal point (`FolderIndex::Journal`) and `FolderTree::watch`
  replays the changes since then (also those made while the app was off) and follows them live, re-reading only the
  folders reported (`FolderIndex::update`); a full scan only without a cache, after a journal reset, more than
  `kMaxReplay` events behind, or on ⌘R (`reindex`). Elsewhere the timed rescans stay. ⌘D picks the drive
  (`FolderTree::setDrive`, until the app quits). The panel is NCD's blue screen (`#142d7f`, white text, a light cyan bar) in both light and dark mode (`Theme::Colors::tree*`).
- **Archives:** opening a .zip/.tar.* etc. expands it next to the archive inside Gifiles
  (`Job::Extract`, undo trashes the result) unless the user set "항상 이 앱으로 열기" for it.
- **Quick Look size:** text, PDF and other content get a base size (900×680) times
  `preview.quick_look_doc_scale`; + / − change it (window up to 90% of the screen, the text keeps scaling
  past that). Sound has its own window (520×260). Dragging the window's edge is remembered per kind
  (`QuickLookWindow::resizeEvent`: any size other than the one the window was given came from the user):
  documents and sound keep that window size (`preview.quick_look_doc_width/height`,
  `preview.quick_look_audio_width/height`, 0 = base; a document's text stays, its + / − scale the stored size too;
  + / − on sound change its size), an image/video grows by as much as the window did and its scale against
  the original becomes `preview.quick_look_scale`. Volume (`preview.volume`) is a slider in the media controls, remembered; a click on the seek or volume bar
  jumps there (`jumpOnClick`); ← / → seek 5 s while sound or video is shown (↑ / ↓ still move the selection).
  The file last played goes on where it stopped when shown again (Quick Look closed, or another file
  shown in between); played to the end, or the browser left its folder (`forgetPlaybackOutside`) → from the start.
  macOS always uses Qt's AVFoundation backend (`PreviewWidget::chooseMediaBackend`, also in the tests): Qt's
  own packages, which CI ships, would pick FFmpeg, where the resume below never goes on.
  On macOS a fresh file seeks only to whole seconds (Qt's AVFoundation player), so `beginResume` seeks to
  the second, plays unseen and muted until the position moves, then seeks to the exact spot.
  Images/videos open at their own size times the remembered scale
  (config.toml `preview.quick_look_scale`, 100% by default; + / − or ⌘+ / ⌘− in Quick Look change it
  in 5% steps and resize the window), capped at 90% of the monitor **the browser window is on**
  (never the panel's own `screen()`), along the side that reaches the edge first. The content is
  fitted up to that scale (`ZoomArea::setFitLimit`); a click toggles 100% (magnifier cursor shows
  the direction; cursors are reset when the window hides — macOS kept the magnifier). The details
  line shows the resolution. No open/close animation.
- **Text size:** ⌘+ / ⌘− / ⌘0 set the file views' font (`view.font_size`, 0 = system); list rows
  scale with the font (24 px at the default size). Gallery icon size is ⌥⌘+ / ⌥⌘− and the slider.
- **Rename editor:** its text starts exactly where the name is drawn (`ItemDelegate::updateEditorGeometry`,
  `QLineEdit#renameEdit` has no padding), so nothing moves when editing starts or Return commits.
  `GIFILES_SNAPSHOT` also saves `*-rename.png` (and `terminal.png`, `folder-tree.png`) to measure it.
- **List rows:** stripes, hover and selection are painted as one rounded pill per row in
  `FileTreeView::drawRow` (QSS item backgrounds are transparent, `show-decoration-selected: 0`),
  so the fold chevron area never splits from the row; each pill leaves a 1px gap below it so
  adjacent selected rows stay separate (rows are 24 px, `kRowHeight`). Names are colored by extension in every view
  (`ItemDelegate::nameStyle`; `Theme::fileColor` / `Theme::folderColor` (bold, packages count as files) read
  config.toml `[file_colors]`, defaults from Mdir III — see docs/history.md; colors are given for dark mode and
  light mode derives them with `Theme::lightModeColor`, never a second setting);
  a selected row/tile in an active view is filled with the item's own color (`ItemDelegate::selectionColor`, `file_colors.selection`);
  on macOS the views' text is `QFont::Light` (Qt's grayscale font smoothing can't be turned off and makes Regular look
  semi-bold; 350 renders as Regular);
  date/size/kind are text at 0.5 opacity; long names elide in the middle.
- **Per-folder view memory:** mode, sort, column sizes and icon size are saved per folder
  (`folders.ini`) only on real user changes; tests reset the scratch folder in `init()`.
- **macOS protected folders:** don't read inside Desktop/Documents/Downloads unless the user opens
  them (e.g. the sidebar uses drawn glyphs, not the folders' own icons) — that triggers permission
  prompts at launch.
- **Include path:** `src` is an INTERFACE include dir only. On case-insensitive macOS a `-I src`
  makes `<util.h>` (forkpty) resolve to `src/Util.h`.
- **Look:** colors come from `Theme::colors()` (light/dark, system accent); icons are line glyphs in
  `Theme.cpp` (SVG has no #AARRGGBB — use stroke-opacity). Don't hard-code colors in widgets
  (exception: the magnifier cursor, black/white so it reads on any image).
- **Recent folders** (`RecentFolders`): only work counts, never browsing — a new undo record
  (`App::recordDone` → `noteRecord`), a file opened (`OpenWith::open` / `openWith`), a selection command
  (`runCommandNow`), a line entered at the shell's prompt (`TerminalWidget::commandEntered`; `cd`, `ls` and
  other looking/moving commands don't count, `isNavigationCommand`). A new kind of work notes its folder
  there too. The list is state (QSettings `recent/folders`, 50 kept); how many show is
  config.toml `sidebar.recent_folders` (0 hides the section). "위치" is no longer always the second
  section: find sections by title.
- **Packages on macOS:** `util::isPackage` asks `NSURLIsPackageKey` (`macIsPackage`), never
  `QFileInfo::isBundle()`: that makes a CFBundle on the GUI thread, which raced with the CFBundles Qt's
  file-info thread makes for the same folders and crashed in `CFBundleGetIdentifier`.
- **Tests must pass on all three platforms:** use the per-platform key modifiers at the top of
  `tests/smoke.cpp` (`kTerminalMods`, …), `np()` for paths as the shell prints them, and no Unix-only
  tools on Windows. In CI no temp dir is special; jobs finish later than locally, so wait for a
  job's result (selection, undo record), not just for files to appear.
- **Tests:** they share one window and run in order; earlier tests can change global state
  (sort order, default mode, terminal). Make new tests robust to that (set the mode/selection
  they need, use their own subfolder, clean up). Use `QPointer` for widgets that delete on close.

## Layout

| Path | Role |
|---|---|
| `src/App` | windows, app-wide undo history, session save/restore, cut state |
| `src/MainWindow` | menus, shortcuts, toolbar, sidebar, tabs, terminal panel and its tabs, AI popup, file operations |
| `src/BrowserTab` | one tab: list (tree) / gallery / column views, history, selection, per-folder view memory (`folders.ini`) |
| `src/FileProxy` | folder-first natural sorting, search filter, Korean columns, drop routing |
| `src/FileOps` | background copy/move/trash/duplicate/extract with undo records |
| `src/ItemDelegate` | row/tile painting, name colors, thumbnails, Finder-style rename editor |
| `src/Preview` | preview widget, `ZoomArea`, the Quick Look window |
| `src/SystemPreview*` | the OS's own preview of office documents & co. (macOS Quick Look, Windows preview handlers) |
| `src/TerminalWidget`, `src/Pty*` | libvterm terminal, pty (forkpty / ConPTY), list integration, queued commands |
| `src/Sidebar`, `src/PathBar` | favorites, recent folders & volumes, breadcrumb / go-to-folder |
| `src/RecentFolders` | the sidebar's "최근 폴더": folders where something was done (QSettings state) |
| `src/FolderIndex`, `src/FolderTree` | the NCD-style folder tree (\`): index of every folder (scan, search, cache file), background scans, the panel over the whole window |
| `src/Settings` | preferences in config.toml (schema, load/watch/save, check) and the settings window (⌘,: 일반/보기/모양 및 색상/미리보기/터미널/선택 항목 메뉴/단축키) |
| `src/Shortcuts` | built-in keys of the menu actions, the user's keys, rebinding |
| `src/Toml` | the small TOML subset config.toml uses |
| `src/Log` | debug log of user actions (dev machine only) |
| `i18n/`, `scripts/i18n.sh` | translations (en, ja, zh_CN; Korean is the source) and their refresh script |
| `packaging/readme.txt` | installed guide to the settings (bundle Resources, next to the .exe, AppImage share/doc) |
| `LICENSE`, `packaging/licenses` | MIT for Gifiles; notices and texts for what the builds ship (Qt LGPLv3 dynamically linked, FFmpeg, libvterm), copied next to readme.txt — keep them in step when a bundled library changes |
| `src/Updater` | automatic updates: signed update.json, download + checks, the swap helper |
| `third_party/tweetnacl` | TweetNaCl (public domain), Ed25519 verification of update.json |
| `src/OpenWith*` | "다음으로 열기" app list and remembered per-extension apps |
| `src/Permissions` | macOS Full Disk Access guide |
| `src/Theme`, `src/MacWindow.mm` | stylesheet, colors, glyphs, file colors; macOS title bar, window animation |
| `tests/smoke.cpp` | end-to-end tests driving a real window with synthetic input, run on all three platforms in CI |
| `tests/unit_widgets.cpp` | single widgets without a MainWindow: preview, media, terminal, sidebar, settings window, "다음으로 열기" |
| `tests/Headless.h`, `scripts/coverage.sh` | every suite on the offscreen platform; coverage of all suites together |
| `tests/unit_fileops.cpp`, `tests/unit_core.cpp`, `tests/unit_update.cpp` | windowless tests: file operations and undo; TOML, settings, command quoting, helpers, the CLI options; automatic updates |
| `third_party/libvterm` | vendored libvterm 0.3.3, built where no system package exists (Windows, Linux CI) |
| `packaging/icon` | app icon: `gifiles.svg` is the source; `.icns` (macOS bundle) and `.ico` (Windows, via `packaging/windows/gifiles.rc`) are rendered from it — re-render both when it changes |
| `packaging/linux` | .desktop file for the AppImage (icon from `packaging/icon`) |
| `docs/history.md` | progress log, decisions, measurements, open items |

## Status

Built and used daily on macOS. Windows and Linux build and pass the tests in CI (offscreen
platform) and ship as release artifacts, but have not been tried by hand on a real desktop yet
(ConPTY terminal, the system "Open with" dialog, PowerShell quoting, Recycle Bin restore on
Windows; `gio` app list, AppImage on various distributions on Linux). Open items are listed in
`docs/history.md`.
