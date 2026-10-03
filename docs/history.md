# Gifiles — history, decisions, open items

Background for whoever continues the work. Rules live in `../AGENTS.md`; user-facing features and
shortcuts in `../README.md`. Dates are 2026.

## How it got here

1. **Go TUI first, dropped.** A terminal Finder clone (Bubble Tea style) came first. Dropped because
   terminals can't do real drag-and-drop or real image/video playback, and behavior became "this
   works here but not there".
2. **Native GUI, Qt 6 Widgets (C++20).** Chosen over web-based stacks for idle memory (the user
   rejected ~100 MB idle web runtimes) and over per-platform native UIs for cross-platform reach.
3. **Named Gifiles (2026-10-02)**, keeping clear of the Finder trademark: repo `zidell/gifiles`,
   bundle id `dev.zidell.gifiles`, `TERM_PROGRAM=Gifiles`, release files `Gifiles-*`. No earlier
   name is left in the code, the settings or the releases.
   App icon added at the same time (option B of three drafts the user saw: the file list with a
   selected row and MIME-colored names over a terminal prompt). Rendered with AppKit from the SVG
   (`NSImage` → PNGs → `iconutil` for .icns; .ico holds PNG entries 16–256).
   Background changed from dark slate to navy the same day (option B3 of three blue drafts): the
   slate blended into dark desktops.

## What exists (all on macOS, 36 end-to-end tests passing)

- **Browsing**: sidebar (favorites with drop-to-insert at position and reorder, volumes), tabs,
  multiple windows (⌘N), back/forward, path bar with go-to-folder, search filter, show hidden.
- **Views**: ⌘1 gallery, ⌘2 list, ⌘3 columns (same numbers as Finder; toolbar buttons in that
  order; default view is still list). The list is an expandable tree (→ expands, ← collapses,
  ⌘↓ enters). Per-folder memory of mode, sort, column sizes, icon size.
- **List look**: rounded full-row pills for stripes/hover/selection (chevron included), names
  colored by MIME group, date/size/kind gray at 0.5 opacity, middle elision for long names.
- **Selection & mouse**: Shift range, ⌘ toggle, rubber band on empty space, drag on a name moves
  files; Return renames (base name selected), Space Quick Look.
- **File operations**: copy/move/duplicate/trash/new folder/new folder with selection/rename/extract,
  all undoable (⌘Z/⇧⌘Z), conflicts dialog (중단/건너뛰기/둘 다 유지/대치), progress + cancel.
  Both copy/paste styles (Mac ⌘C→⌘V/⌥⌘V and Windows Ctrl+X→Ctrl+V). Nothing is deleted
  permanently.
- **Archives**: opening one expands it next to itself inside Gifiles (Archive Utility rules: one
  top item comes out as is, several go into a folder named after the archive), selected after;
  undo trashes it. Uses `ditto` for zip and `tar` otherwise on macOS.
- **Open With**: "다음으로 열기…" with the app list and "항상 이 앱으로 열기" per extension.
- **Preview**: preview pane and Quick Look (images incl. animated GIF, video/audio with controls,
  text, PDF, info card). Quick Look sizes images/videos between 60% and 90% of the browser
  window's monitor; click toggles 100% with a magnifier cursor (+/−), ⌘+/⌘− zoom, drag/scroll to
  pan, resolution in the details line, full-resolution decode only when zoomed past the
  screen-sized copy. No open/close animation.
- **Terminal panel** (libvterm + the login shell; PowerShell on Windows): header bar always at the
  bottom, fold/unfold by chevron, bar click, ⌃\` (⌃₩) or by dragging the edge; follows the
  browser's folder when idle, the shell's cd moves the browser, items dragged onto it are typed
  in as quoted paths. Korean names display correctly (wide-char continuation cells skipped, NFC).
- **AI button** (✦, ⌥⌘A, context menu): one-line prompt → runs `claude -p` / `codex exec` /
  custom template in the terminal with the request plus selected paths (Settings → AI).
- **Settings (⌘,)**: 일반 / 보기 / 모양 / 미리보기 / 터미널 / AI / 단축키, all stored in `config.toml`
  (see Decisions). 단축키: every menu action, "재지정" captures the next key.
- **Open key on files** (⌘↓/⌘O with one file or several items): shows the context menu with
  "열기" / "열기 (N개 항목)" first and highlighted (Return opens) — the user does something else with
  selected files more often than opening them. A single folder is still entered at once.
  Context menu "선택한 항목들로…": the user's own terminal commands (Settings → 선택 항목 메뉴,
  config.toml `[selection_menu]`), defaults 새로운 폴더 / 압축 파일 생성 / AI로 실행, with {files}
  {names} {dir} {prompt}. The user's idea: register shell scripts there to run on any selection.
  A built-in zip job came first and was dropped: Windows `tar.exe` could not write a Korean
  archive name ("압축 파일.zip" → "?? ??.zip"); PowerShell's Compress-Archive in the command can.
- **macOS integration**: signed Release build so Full Disk Access sticks; first-launch FDA guide
  with a one-click jump to System Settings; unified title bar (or native one via settings).

## Decisions and why

- **Settings in config.toml (2026-10-02)**, following the user's policy repo
  github.com/zidell/agent-configuration-accessibility: one TOML file in the user's config folder
  (macOS `~/Library/Application Support/Gifiles`, Windows `%APPDATA%\Gifiles`, Linux
  `$XDG_CONFIG_HOME/gifiles`) that explains every item (type, allowed values, unit, default, when it
  applies), written whole by the app with those comments, re-read on outside edits; wrong values
  are reported and the previous value kept; a syntax error keeps everything. Found via the shipped
  `readme.txt`, `--help`, `--config-path`; `--print-default-config` and `--check-config` for agents.
  TOML over INI because QSettings' INI writer drops comments. Shortcuts are `[shortcuts]` with the
  built-in keys commented out (a later change of a default still reaches users who never changed
  that key). Session/window state stays in QSettings (state, not settings). The user's existing
  values (folders first off, icon size 84, favorites) were moved into the file by hand once and
  the old QSettings keys deleted; no migration code.
- **Debug log (2026-10-02)**: the user asked for a log of every action to analyse reports. Only on
  the build machine (the checkout's `CMakeLists.txt` must exist), `logs/` in the repo, one file per
  day, 7 days kept. Typed text in fields and the terminal is counted, not recorded (passwords).
- **Rename survives a re-added row (2026-10-02)**: Linux CI failed `newFolderStartsRename` three
  times in a day: right after a new folder appears, the file watcher drops and re-adds its row,
  which closed the rename editor (typing and Return then went nowhere). The delegate now keeps
  the text of an editor that closes without Return/Esc (own event filter: the view removes the
  delegate's before hiding a closing editor), and the selection restore reopens it with that
  text (`renameSurvivesRowReAdd` simulates it with a search-filter round trip).
- **i18n (2026-10-02)**: the user asked for English, Korean and the major languages; agreed on
  ko/en/ja/zh_CN. Korean stays the source text (`Gifiles::tr`), so the code and the tests didn't
  change wording; ~360 messages, first translations drafted by translation agents (en, ja, zh_CN)
  and not yet reviewed by a native speaker. Shortcut ids and `[shortcuts]` keys stay Korean in
  every language (stable config keys); config.toml comments and `--help` follow the UI language.
- **Quick Look opens at the natural size (2026-10-02)**: the user preferred the original size over
  "fill 60–90% of the screen"; + / − set a remembered scale (`preview.quick_look_scale`), still
  capped at 90% of the screen. ⌘+ / ⌘− in the browser became text size (they did nothing in the
  list before; gallery icon size moved to ⌥⌘+ / ⌥⌘−). The magnifier cursor stayed up after Quick
  Look closed on macOS; cursors are now unset and the arrow set on hide.
- **Row pills 1px apart, rows 24 px (2026-10-02)**: adjacent selected rows ran together into one
  block; the user wanted each row visible. Rows went from 28 to 24 px at the same time.

- **Terminal selection insertion removed.** Originally the selected items were typed into the
  command line live while the user was typing a command. The user changed it: paths go in only
  when items are dragged onto the terminal.
- **Terminal button removed from the toolbar**; the panel's own bar (always visible at the bottom)
  is the control. First version only capped the panel height, leaving the bar floating mid-window
  — the user was (rightly) annoyed; the fix gives the space back to the list. An animated slide
  was tried and rejected ("애니메이션은 필요없고").
- **Quick Look fade removed** (`NSWindowAnimationBehaviorNone` via `macDisableWindowAnimation`):
  "속도가 더 중요".
- **Quick Look sizing against the browser's monitor**: with two monitors the panel's `screen()` is
  the primary display until shown, which made sizes jump between items.
- **Quick Look appears at its final size** (2026-10-01, "움찔거리지", "여전히 작게 나오는 경우"):
  the window used to show at the default size (or the previous item's) with "불러오는 중…" and
  resize only when the background decode finished — 40–340 ms for a big JPEG/HEIC, longer on
  slow disks. Now the image size comes from the file header (`QImageReader::size()`, Retina from
  the PNG pHYs / JPEG JFIF dpi via `headerDpi`), a video's from the container metadata, and a
  video's first show waits for it (≤ 500 ms, `QuickLookWindow::present`). A window not shown yet
  has no layout to measure, so the chrome around the viewport is added up
  (`PreviewWidget::chromeEstimate`).
- **Terminal tabs** (2026-10-01, "대기 상태가 아닐 때는 새로운 탭을 열어"): the panel bar got
  Chrome-like tabs. A browser move goes to the current tab; if a program runs there, a new tab
  opens at the folder instead of waiting forever. A half-typed line deliberately still waits
  (typing `rm -rf ` and then browsing to find the files to drag must not jump to another tab).
  A freshly started shell counts as not busy until its first prompt, so rc files don't spawn tabs.
  Switching terminal tabs moves the browser to that shell's folder. The AI button also runs in a
  new tab when the current one is busy. Closing a tab with a running program hung the app:
  `Pty::~Pty` waited for the shell before closing the master; it closes the master first now.
- **Rename editor position** (2026-10-01, "엔터를 눌렀을 때 글자가 정확히 그 위치에"): the editor's
  text sat ~6 pt right of the drawn name (QLineEdit QSS padding + frame). Now the editor has no
  padding and is placed so its text origin matches the drawn text. Measured on real rendering
  (`GIFILES_SNAPSHOT` → `*-rename.png`): list within 1 device px (0.5 pt, can't place widgets
  finer), columns exact (needed −1 px for the column lists' item padding), gallery exact
  vertically (editor moved up 2 pt) and within 1 device px horizontally.
- **Preview font sizes** (2026-10-01): Settings → 미리보기 has a text size (code/config, fixed
  font, default 12) and a document size (txt/md/rst and extension-less names except Makefile etc.,
  UI font with wrapping, default 14). The user picked "md·txt as documents" over rendering Markdown.
- **"Finder에서 열기" in the context menus** (2026-10-02): replaces 정보 가져오기 there (still in the
  File menu, ⌘I, on all platforms — Windows had dropped it for "탐색기에서 열기"). Folder → opens it;
  file → its folder with the file selected (`open -R`, `explorer /select,`, Linux: freedesktop
  `FileManager1.ShowItems/ShowFolders` over `dbus-send`, so the desktop's own file manager; falls
  back to `xdg-open` on the folder). `util::revealCommand`. Not launched in tests (it would open
  windows on the desktop); only the commands are checked.
- **Name colors**: first per extension (hash → hue), then per MIME type at the user's request, as
  groups ("그룹핑이 좀 필요할 듯"). Office files are checked before archives because their MIME
  types inherit from zip. Per-type hue offset kept to ±8° so groups (esp. image orange vs archive
  yellow) stay distinguishable. FNV-1a, not qHash (seeded per process).
- **Secondary columns**: 0.7 opacity, then "더 투명" → 0.5.
- **Row selection painting** moved from per-cell QSS into `drawRow` because the branch (chevron)
  area was painted separately and split the rounded highlight.

## Measurements

- **List row background ownership** (2026-10-02, Linux): reported selection missing on one
  stripe parity. Native Wayland screenshots of the basic two-row fixture did not reproduce it.
  Removed Qt's native alternating-row pass and the palette workaround; `FileTreeView::drawRow`
  now owns stripes as well as hover and selection. Pixel tests check every column on both
  selected parities with and without list focus. Real trackpad selection still needs confirmation.

- **Windows terminal smoke failures** (2026-10-02): a dropped-path assertion compared a
  literal separator that `screenText()` trims when it falls at a soft wrap. Check the quoted
  arguments individually and keep the executed echo check. Terminal tests now wait for the
  first prompt and each test closes its terminal tabs, so an early failure cannot leave a
  typed command or open panel in the next test. PowerShell's OSC 7 marks the first prompt,
  including when its directory is unchanged; queued cd/AI commands wait for that readiness
  instead of treating a process with no children during startup as ready. This also fixes a
  command started before the first poll never being recognized as busy.
  Separately reproduced libvterm 0.3.3's resize abort with 200 wrapped characters on a 3x80
  screen resized to 2x10. Its clipped-line cursor check excluded the last row; fixed the
  inclusive boundary and added a regression checking cursor bounds and subsequent output.
  The patched bundled library is now the build default, and the macOS rebuild script also
  selects it for existing caches. Windows full smoke: Debug 33 passed, 0 failed (22.7 s),
  Release 33 passed, 0 failed (10.6 s). The deployed Release folder contains only the native
  Windows platform plugin, so offscreen testing also sets `QT_QPA_PLATFORM_PLUGIN_PATH` to
  the Qt toolchain's `plugins/platforms`; without it Qt shows a startup error dialog.
  Rebuilt and relaunched the Windows Release app and confirmed its window is visible.
  Other operating systems and physical keyboard input were not run in this Windows session.

- **Terminal bell muted** (2026-10-02, Linux desktop): folder navigation triggered the shell's
  BEL through automatic cd, which the terminal forwarded to `QApplication::beep()`. The BEL
  callback now consumes it silently. Smoke coverage feeds BEL mixed with text and then checks
  parent/child folder following; audible output itself isn't measured by the offscreen test.

- **Idle memory** (Release, one window ~1200×840 pt on a Retina external monitor, terminal folded):
  82 MB phys_footprint. 46 MB of it is IOSurface — Qt keeps three window-sized backing buffers
  (scales with window size); ~28 MB malloc; the rest code/system. A bare Qt window measured ~58 MB
  earlier. The user finds it "생각보다는 먹네"; offered (not yet done): release buffers when the
  window is hidden/minimized (expected tens of MB, unmeasured) and cap caches (≤10 MB).
- **Windows / Linux builds (2026-10-01).** Repo `zidell/gifiles`; GitHub Actions builds
  Windows (MSVC, Qt 6.10.1 via aqt, `windeployqt` zip), Linux (Ubuntu 22.04, AppImage via
  linuxdeploy) and macOS, runs the smoke tests offscreen on all three, and a `v*` tag publishes a
  release. libvterm is vendored (`third_party/libvterm`, launchpad downloads failed with 502).
  Running the tests on the other platforms found real bugs, fixed for all platforms:
  - ⌘Z pressed while a file job was still running undid the *previous* operation (the job's
    record arrives when it finishes). Undo/redo now wait for running jobs (`m_deferredUndo`).
  - The terminal's echo of our own `cd` moved the browser back when it had already moved on
    (browser → A → B quickly ended on A). Our own cd's arrival no longer emits `cwdChanged`;
    OSC 7 (PowerShell) goes through the same check (`TerminalWidget::shellMovedTo`).
  - A per-folder view change saved by the 300 ms debounce after navigating was written under
    the new folder's key. Pending saves are flushed before the path changes.
  - ConPTY: with redirected std handles (CI, or started from a terminal) PowerShell inherited
    them and exited at once; `STARTF_USESTDHANDLES` keeps it on the pseudo console.
  - Natural sort (file2 < file10) failed in the C locale (QCollator ignores numeric mode
    there); a hand-written comparison is used for that locale.
  - Quick Look's chrome guess was 14 px short on Windows; the real viewport is measured once
    after sizing and corrected.
  - The error box after a failed job used `exec()` (a nested loop); now window-modal `open()`.
  CI-only quirks: Qt's XDG trash needs `~/.local/share` to exist (fresh runners lack it); Windows
  `zip` is `tar -a`; Linux `/bin/sh` is dash (no `$'...'`), so the test uses bash.

- **BetterTouchTool** (modifier+drag move/resize "works only sometimes", 2026-10-01 root cause):
  BTT finds the window under the mouse with an accessibility hit test. Qt's
  `QMacAccessibilityElement accessibilityHitTest:` returns `NSAccessibilityUnignoredAncestor(child)`,
  which is nil over the item views (file list, sidebar rows, the whole tab page), and AppKit reports
  that as `kAXErrorNotImplemented` (-25208). Measured on a real Downloads window: 108 of 144 grid
  points failed (Finder: 0); it "worked sometimes" because toolbar, path bar, status bar and the
  empty sidebar area did resolve. An earlier probe (289 points all OK) had not covered the list.
  Fix: `macFixAccessibilityHitTest()` (MacWindow.mm) swaps QNSView's `accessibilityHitTest:` for a
  wrapper that falls back to the window when Qt returns nil. After: 144/144 resolve, max 2 ms.
  Not verified with a real BTT drag (that would drive the live desktop). If it still fails, check
  BTT's log (`~/Library/Application Support/BetterTouchTool/Logs`, "Slow accessibility response").
  A setting "macOS 기본 제목 막대 사용" existed for title-bar-drag based tools; removed 2026-10-03 at the
  user's request (no longer needed after the hit-test fix); an old `appearance.native_title_bar` line is ignored.
- **Quick Look sizing trace** (2026-10-01, real Cocoa, external 3008×1667 + built-in 1680×1025
  pt, browser on either, moved between them while open): before the fix every image/video got one
  resize *after* appearing at 944×600 or the previous size; the final sizes already followed the
  60–90% rule on both monitors (no wrong-size case found). After: the window appears at the final
  size, one `setGeometry` per item, chrome estimate = measured (26 pt image, 55 pt video). A
  temporary `GIFILES_QLTEST` hook in `main.cpp` drove it (not committed).

- **2026-10-02 settings and menus round:** 재지정 opens a capture window (key shown in the middle,
  확인/취소 buttons because Return/Esc can be shortcuts; 초기화/전부 제거 confirm). The AI page is gone:
  the AI button runs the selection menu's built-in `ai` command. Selection commands gained `key`
  (menu letter), `terminal` (false = quiet QProcess run, failure-only report) and `id` for the three
  built-ins, which can't be removed; the page is a list with 추가/편집/삭제/위로/아래로 and an edit
  dialog. Context menus show fixed Windows-style letters "(O)". ⌘W with Settings/Info/Quick Look in
  front closes that window (macOS menu bar keys reached the main window). Quick Look: non-image
  content has its own remembered scale (window + text), media volume slider remembered.
  Later the same day the toolbar AI button and its action (⌥⌘A) were removed; the {prompt} box
  opens centered on the window.
  Office files in Quick Look ("기본 os에 있는 기능은 최대한 끌어써야지"): the user wants the OS's own
  features used wherever they exist. Our renderers stay for images/video/sound/PDF/text (sizing,
  zoom, volume are ours); office documents and whatever would end as an info card go to the OS
  (`SystemPreview`): macOS `QLPreviewView` (docx and xlsx checked in `quick-look.png` snapshots),
  Windows `IPreviewHandler` (CI build only), Linux LibreOffice → PDF in the cache (not run anywhere
  yet; the CI runner has no LibreOffice). Libraries were weighed and dropped: LibreOfficeKit
  (~300 MB), OpenXLSX/DuckX (values/text only), pandoc, a web view (rejected earlier), Aspose (paid).

- **Folder color, lighter text (2026-10-02)**: folders get their own blue and bold in all three views ("폴더까지
  감안해서 색 밸런스"); documents moved from blue (212°) to teal (185°); folder blue a bit lighter than the
  kinds (blue reads darker). Light mode colors darkened (L 88 → 66) — "너무 안 보인다"; dark mode kept ("딱 좋아").
  "기본 파일도 약간 볼디": Qt's CoreText engine always applies grayscale smoothing (stem darkening);
  `NoSubpixelAntialias`, `-AppleFontSmoothing 0` and `CGFontRenderingFontSmoothingDisabled` changed no pixel
  (measured on snapshots). Weight 350 renders as Regular; `QFont::Light` for the file views on macOS.

- **Media controls (2026-10-02)**: the volume slider touched Quick Look's right edge (the window has no
  margins) — the controls row got its own (10/16 px), checked on a snapshot. A click on the seek/volume
  groove jumps there (Qt only pages); ← / → seek ±5 s while media is shown.

## Open items

- BTT move/resize: confirm with the user that the AX hit-test fix (above) solved it.
- Memory: try releasing window buffers while hidden/minimized; measure before/after.
- Quick Look "작게 나오는 경우": no wrong final size reproduced (see the trace above); if the user
  still sees one, get the file type and which monitor.
- Verify on a real screen what tests can't: real mouse drags of the
  terminal edge, the AI button with the real `claude`/`codex` (only a stand-in command is tested),
  ⌃\` under the Korean input source.
- `claude -p` doesn't edit files by default; the user may want a custom template like
  `claude -p --permission-mode acceptEdits {prompt}`.
- Windows/Linux: try the release builds by hand on a real desktop (ConPTY terminal with Korean
  names, Recycle Bin restore, "Open with", AppImage on a few distributions).
- Not yet seen on a real screen: the multi-selection menu from ⌘↓, the config-problem notice,
  menu letters under the real Korean input source, ← / → seeking with real playback (no media test), a quiet
  command failing on Windows. (Settings pages, capture and edit dialogs, context menu: snapshots.)
- System preview: on Windows try docx/xlsx/pdf with Office installed (handler found, drawn, resized,
  DPI); on macOS, a click into the Quick Look view then Space should still close it (key monitor; not
  tried with real input); Linux LibreOffice conversion by hand. Gallery thumbnails could also come from
  the OS (QLThumbnailGenerator / IThumbnailProvider) for office files.
- i18n: dates still use the Korean pattern ("2026. 10. 2. 14:05", `util::humanDate`) in every
  language; translations were drafted by agents and need a native read (ja, zh_CN especially).
- The debug log was asked for "about a day": ask the user whether to keep it on after looking at it.

## Linux desktop details (2026-10-02)

- **Linux dark selection** (2026-10-02): active file selection backgrounds use 80% of the system
  accent RGB channels to improve white filename contrast, consistently in list/gallery/columns.
  Light mode and other platforms retain the system accent. Buttons and other accent UI keep their
  original colors. Smoke coverage checks platform/mode scope and dark selected-row pixels.
  Native Wayland Release measurement: selected-row RGB changed from (61,174,233) to
  (49,139,186), within rounding of 80%; cropped screenshot inspected. Debug and final Release
  runs pass 31 tests. One earlier Release run crashed during archivesExpandInPlace in
  QSortFilterProxyModel::parent while QTreeView painted; a full retry passed. Root cause remains
  unconfirmed and is recorded for follow-up. The Release app remains running.

- **Linux dark selection adjustment** (2026-10-02): user requested another 15% reduction from
  the previous selection background. Active selection now uses 68% of the original system
  accent RGB (0.80 × 0.85), across list/gallery/columns. Platform/mode regression expectations
  updated accordingly.
  Verified: Debug build and 31 smoke tests passed; Release rebuilt and relaunched. Native
  Wayland selected-row pixels changed from (49,139,186) to (41,118,158), another 15% reduction
  within RGB rounding; cropped rendering inspected.

- **Updated application icon** (2026-10-02): imported the navy icon and its .icns/.ico variants
  from origin/main. Embedded the SVG as the window icon; wired macOS and Windows
  native icons and Linux packaging to the same source. Linux menu/startup registration uses
  a content-versioned icon name so desktop caches load the replacement immediately.
  Verified: imported assets byte-match origin/main, Qt native SVG rendering inspected, Debug
  smoke retry passes 32 tests, Release resource rendering test passes. Menu and login entries
  point to the installed new icon; desktop metadata cache refreshed and Release relaunched.
  The first full smoke run hit the previously recorded archive/tree proxy crash; retry passed.

## Synchronize Linux checkout with main (2026-10-02)

- Merged upstream features with the local row-background fix, 68% Linux dark selection,
  versioned icon cache and stable `dev.zidell.Gifiles` desktop ID. Windows selection retains
  upstream's darker tone. Removed the completed legacy-settings migration as upstream intended.
- Updated config-file stripe tests for custom row painting. Fixed a newly reproduced Linux
  crash when closing an office preview during LibreOffice conversion: stop and disconnect
  conversion processes before preview child widgets are destroyed. Added shutdown coverage.
- Verification: Debug and Release each pass 52 smoke tests. Native Wayland Release snapshots
  and a list crop inspected; selected-row RGB remains (41,118,158). Release relaunched.
  Physical input and macOS/Windows behavior still depend on their own verification/CI.

- First sync CI: Linux and macOS passed. Windows exposed two test issues: QColor's HSV
  selection and captured RGB pixels compare unequal despite identical RGBA, and the new
  terminal's initial cwd was mistaken for a ready PowerShell prompt. Compare rendered RGBA
  and wait for the new terminal's first prompt before checking subsequent folder following.

## Configurable app shortcuts and pane focus (2026-10-02)

- App-defined shortcuts in the file views, Quick Look, terminal, sidebar, search, path input,
  rename editor and context menus now share the shortcut settings registry. Contexts permit
  local reuse without removing unrelated bindings. Plain Return also accepts keypad Enter.
- Defaults follow Finder, with Command mapped to Ctrl on Windows/Linux. Only Windows uses
  Enter to open and F2 to rename; parent navigation remains Command/Ctrl+Up on all platforms.
  Removed the old Alt history/parent aliases. Terminal tab creation/closing follows the same
  Command/Ctrl+T/W defaults and can be reassigned. OS key remapping was not changed.
- Alt+Left focuses/reveals the sidebar, Alt+Right/Up focuses files, Alt+Down focuses/unfolds
  the terminal, preserving selections and partially typed shell input. Column focus prefers
  visible columns over recycled hidden columns. Down with no selection starts at the first
  item, including entry from search.
- Menu shortcut combinations are painted at 50% opacity while labels/icons/backgrounds retain
  their original rendering; pixel coverage checks both selected and unselected menu rows.
- Updated documentation and English/Japanese/Chinese shortcut labels. The development snapshot
  hook returns to list mode before opening its context-menu capture.
- Verification: Debug and Release each pass 57 smoke tests, including Windows-specific default
  expectations, pane navigation, empty-selection entry and contextual reassignment. Native
  Wayland settings/list/context-menu rendering inspected; the menu label peaks at RGB
  (242,242,244), while shortcut text peaks at (142,142,145), halfway to its background.
  Real-menu widget coverage includes the QSS style
  path, which bypasses the underlying proxy style. Final targeted checks pass in both builds.
  Physical input on Windows/macOS was not tested here.
- Stabilized existing Windows terminal checks: accept the shell's actual tab title, and verify
  dropped-path command arguments from its output file rather than terminal soft-wrapped cells.
  Isolated the rename/re-added-row test's folder from earlier shared-window state.

## Sidebar cursor and current-folder highlight (2026-10-02)

- Returning from files to the sidebar keeps its last keyboard cursor instead of resetting to
  the current-folder favorite or first favorite. Sidebar rebuilds also restore that cursor by path.
- The current folder defines the selected sidebar row independently of keyboard navigation.
  A different row under keyboard focus gets only the subtle hover background; it loses that
  background when focus returns to files. Arrow movement does not navigate or change the open
  folder's selection. Enter/click still opens the cursor's folder.
- Added regression coverage for Alt-arrow and plain-arrow round trips, file selection retention,
  favorites rebuilds, Enter activation, and rendered current-folder/focus/background colors.
  Debug and Release each pass 58 smoke tests; final focused checks pass after guarding rebuild
  signals. Native Wayland Release focused/unfocused sidebar crops inspected: A keeps the active
  highlight while B gets only a faint background during keyboard focus. No live desktop input
  was driven; Windows/macOS physical input was not tested here.

## File name colors from Mdir III, editable in 색상 (2026-10-03)

- The user wanted the old Korean DOS file manager Mdir's colors. Mdir III 3.10's own distribution
  (archive.org `m3v310`, 1998) was read: `M.CFG` / `BLACK.COL` attribute bytes per extension, the
  manual `M.DOC` (lines 473–476: BAT yellow, COM light cyan, EXE light green, directories red,
  hidden files purple) and DOSBox screenshots (`m_000.png`, namu.wiki's Mdir II capture), sampled.
  Defaults on a black background: EXE light green #55FF55, COM light cyan #55FFFF, BAT/BTM/LNK
  yellow #FFFF55, archives (ZIP ARJ LZH RAR …) light magenta #FF55FF, documents (DOC HWP TXT …)
  cyan #00AAAA, temp/backup (BAK $$$ TMP) red #AA0000, source (PAS C CPP ASM BAS) light blue
  #5555FF, H/OBJ/LIB blue #0000AA, pictures and video (GIF PCX JPG MPG AVI …) green #00AA00,
  sound (VOC MOD MID WAV MP3 …) brown #AA5500, everything else light gray, directories light red
  #FF5555, drive names brown. (The r-mdir remake's orange folders are not the original's.) The
  user's own recollection of the screen then set the defaults where it differs: executables lime,
  source code purple (headers included; objects/libraries stay blue), folders red close to a dull
  orange (then: folders default to the normal text color, `folder = ""`; Mdir's #CD6A51 is one
  setting away), and everything slightly dull, not vivid. Kinds Mdir didn't have went to the colors it left
  free: video a cooler green than pictures (pictures then set by the user to sand #D7CA8C), data/config/db sky blue, shortcuts (lnk, url, webloc)
  cyan, fonts and design/3D files rose. Grouped by what files are for (the user: "용도별로"): COM
  joined the programs; installers (apk, ipa, dmg, pkg, msi, deb, rpm) first went with the archives
  (they are zips/disk images), then to the programs at the user's choice; iso/img/vhd stay archives. Defaults are HSL ~80–95% saturation (first ~40–55%: too dull, the user said; then +20, +10, +10 points), ~55–70% lightness.
- Colors moved from MIME groups (`kindHue`) to config.toml `[file_colors]`: `folder = "#RRGGBB"` and
  `groups = [ { extensions = "zip, 7z", color = "#RRGGBB" }, … ]`, first group naming an extension
  wins, unlisted extensions keep the plain text color. Defaults keep Mdir's hues with modern
  extensions added, softened for a dark background (pure DOS colors glare or vanish).
- One color per entry, given for dark mode; light mode derives it (`Theme::lightModeColor`:
  lightness ×0.55, saturation ×1.5). First tried ×0.38 / ×4/3 (the ratio between the old vivid
  dark/light pairs, 178→66): with the muted defaults it gave near-black names whose hues couldn't be
  told apart on white (design/backup/font all dark maroon).
- Settings → 색상: the folder row, then one row per group (extensions line edit drawn in the
  color as it shows in the current theme, swatch → QColorDialog, hex field, 삭제), 추가, and 초기화
  at the bottom (asks first; removes both keys).
  The swatch opens a non-modal-to-the-app `QColorDialog` (`open()`): every color it points at shows
  in the file views at once (`Theme::previewFileColor` / `previewFolderColor`, over the settings, nothing
  saved); 확인 stores it, 취소 drops the preview.
- Pre-existing: `listSelectionHighlightsBothRowParities` and `dragOnEmptySpaceSelectsRows` fail in
  the full local macOS run (also on the clean HEAD before this change) and pass alone.
- 모양 → 파일 이름: "굵게 보기" (`appearance.bold_names`, every name bold, not only folders; the rename
  editor goes bold too so the text doesn't move) and "항상 대문자로 표시" (`appearance.uppercase_names`,
  `ItemDelegate::initStyleOption` upper-cases the shown text only; the model and the rename editor keep
  the real name). Both off by default.

## Developer-machine scripts out of the repo (2026-10-03)

- The repo may become public, so what only the developer's computers need left it:
  `scripts/rebuild-run.sh` and `scripts/register-linux.sh` became one `local/rebuild-run.sh` in a
  gitignored `local/` folder of each checkout (the Linux machine needs its own copy), with the rules
  that went with it in `local/AGENTS.md` (rebuild/relaunch after every change, keep the app running,
  signing team). AGENTS.md keeps a one-line pointer.

## Selection in the item's color (2026-10-03)

- Like Mdir's selection bar: a selected item in an active view gets its own file/folder color as the
  background (the color as given, in both themes) with one black text (`Theme::selectionTextColor`);
  items without a color (folders by default) get light gray `Theme::plainSelectionColor` (first the
  accent with white text: the user wanted one black everywhere). `file_colors.selection` (default on,
  색상 tab checkbox; off = the accent as before).
  List: `FileTreeView::drawRow` fills the pill, the delegate draws the text as unselected on it;
  columns: the delegate fills its own pill; gallery: the name pill. Inactive selection stays gray.
  Checked on real rendering in list and gallery (dark/light); column view not looked at on screen.
  The list draws a selected folder's chevron itself in that black (`FileTreeView::drawBranches`);
  the style's white selected chevron vanished on the light gray.

## Color weight balanced in OKLab (2026-10-03)

- The user: colors looked unequal in weight (some heavy, some light) and light mode was hard to read.
  Measured in OKLCH, the defaults' perceived lightness L ran from 0.55 (build blue) to 0.90 (yellow);
  equal HSL saturation/lightness does not mean equal weight.
- Defaults now share L 0.78 with chroma min(0.19, 92% of what sRGB allows at that L and hue); hues
  kept. Image sand stays low-chroma (0.085), the user's pick. Trade-off: at one lightness yellow
  can't be bright lemon and red/blue turn pastel (backup salmon, build periwinkle).
- `Theme::lightModeColor` now works in OKLab too: every color gets L 0.52 on white, its hue, chroma
  ×1.1 cut to the sRGB gamut. The HSL ×0.55 rule left yellow/cyan pale and blue near black.
- Found while checking: the app writes every item on save, so the user's config.toml had pinned an
  older default list and none of the later default changes showed in the running app. Unchanged
  `file_colors.groups` is now written commented out (like `sidebar.favorites`), so it keeps following
  the app's defaults; it is set only once the user edits a color.
- Settings: 모양 and 색상 merged into one "모양 및 색상" tab (theme and name switches on top, colors below);
  the window grew to 760×620 so the color rows keep room.

## Public repository (2026-10-03)

- Checked before making the repo public: no credentials anywhere in the history (key/token/password
  and private-key patterns, removed lines included), no Actions secrets or self-hosted runners, and
  `local/`, `logs/`, builds never committed. The author e-mail in commits and the Apple team id in
  old diffs stay (the user is fine with both; the team id is in any signed app anyway).
- License: MIT (`LICENSE`). Qt is LGPLv3 and linked dynamically, which MIT code may do; what the
  LGPL asks of the shipped builds is met by `packaging/licenses/` (notice + LGPL-3.0/GPL-3.0 texts
  for Qt, LGPL-2.1 for the FFmpeg Qt Multimedia ships, libvterm's MIT text), copied next to the
  .exe, into the AppImage's share/doc and the bundle's Resources. The Qt modules used are all
  LGPL ones (no GPL-only modules such as Charts).
- CI token: `contents: read` for every job, `write` only for the tag release job.
- The 0.1.0 release (built under the old name) was removed with its tag, the wiki turned off.
- History reset: the repo went public with one commit holding the tree as it was; the 56 earlier
  commits (old names in their messages and diffs) are kept only as a git bundle in the macOS
  checkout's `local/gifiles-history-2026-10-03.bundle` (`git clone <bundle>` to bisect or blame the old work).

- Other checkouts (the Linux machine) can't pull across the reset: `git fetch && git reset --hard
  origin/main` once, after saving any local work.

## Automatic updates (2026-10-03)

- The user asked for automatic updates on every OS because releases go out often. Decided with the
  user: every push to main that passes on all three platforms becomes a release `v0.1.<run number>`
  (no hand-made tags any more); a new version is downloaded in the background and installed on quit
  or by the toolbar's 업데이트 button (restart); macOS included, signed and notarized in CI.
- Why not Sparkle/WinSparkle/AppImageUpdate: three different systems, feeds and keys for one small
  job. One `Updater` covers all three: `update.json` (version + per-platform file, SHA-256, size)
  signed with Ed25519. TweetNaCl (public domain, ~800 lines) verifies it — Qt has no Ed25519 and
  OpenSSL isn't on every platform. The release job signs with `openssl pkeyutl -rawin`; a test
  checks an openssl-made signature verifies with TweetNaCl.
- Why sign at all when the download is HTTPS from GitHub: a token that can upload release files
  can't read the signing secret, so a stolen token can't push an update. On macOS the unpacked app
  must also pass `codesign --verify --deep --strict` with the identifier and team requirement.
- The swap: a helper script waits for the app's process to exit (up to 2 minutes), copies the new
  version next to the old one, renames the old away, renames the new in, puts the old back on
  failure and relaunches only for "restart now". Windows: PowerShell with retries (files stay locked
  for a moment after exit); macOS: `ditto` keeps the signature; Linux: replaces `$APPIMAGE`.
- Off in builds by hand and in tests (`Updater::allowUpdates` is only called by `main.cpp` when
  built with `GIFILES_UPDATES`), so the developer's own build from this checkout never updates.
- Signing key: generated 2026-10-03, private half in `local/update-signing-key.pem` (gitignored) and
  the repo secret `UPDATE_SIGNING_KEY`; public half in `Updater.cpp`. The user backed the private
  key up outside this Mac the same day (a GitHub secret can't be read back). Losing it means installed
  copies can't be updated any more except by a manual download.
- Not verified yet: a real update end to end on each OS (needs two published releases); the
  Windows helper on a real desktop; macOS notarization in CI (needs the certificate and API key
  secrets from the user).

## Test coverage widened, bugs it found (2026-10-03)

- Asked by the user before going public. Line coverage of src (smoke + unit tests, llvm-cov) went
  from 78% to 87.5%: FileOps 62→95%, Toml 76→100%, Settings 85→89% (the dialog is most of the
  rest), Sidebar 58→93%, PathBar 59→100%, FileProxy 59→93%, App 66→90%. New windowless suites:
  `tests/unit_fileops.cpp` (54), `tests/unit_core.cpp` (168, also runs the CLI), `tests/unit_update.cpp`
  (17); `tests/smoke.cpp` has 81. Still low: Log (dev machine only), Permissions (macOS dialog),
  OpenWith (73%, system app lists).
- Real bugs found and fixed:
  - Data loss: undoing a move or a trash onto a folder that took the old name since deleted that
    folder permanently (`moveItem` fell back to the cross-volume path, whose cleanup removed the
    destination). `moveItem` now refuses an existing destination.
  - Data loss / security: the built-in zip command read a selected file named "-m" as an option and
    deleted the other originals. `{names}` entries starting with "-" now get "./" (every command).
  - A recalled history line (↑, Tab, ⌃R) was replaced by the browser's cd: the terminal only knew
    typed text. `m_lineRecalled` holds the cd until Enter or ⌃C.
  - Replace onto a folder holding the source trashed the source too (now refused, as Finder does);
    copying an unreadable folder succeeded with its contents missing; relative symlinks became
    absolute in copies; a canceled undo/redo lost the steps it hadn't reached (`OpResult::rest`
    goes back to its stack).
  - PDFs showed as a first-page image (Qt PDF's image plugin won before the PDF viewer); broken ones
    said "이미지를 열 수 없습니다".
  - Tabs of a window opened with several folders came out reversed (session restore too); a
    restored session with a vanished folder applied views and the current tab by the old positions.
  - The open folder renamed away and back from outside left the list showing "/" (`checkRoot`).
  - Opening the preview pane showed what it had before, not the current item.
  - Settings: unknown key names in [shortcuts] passed the check (Qt parses them as Key_unknown);
    a wrong [shortcuts]/[open_with] value dropped the previous one; "#RRGGBB\n" passed as a color;
    non-string label/command/key in selection commands passed; sizes just below a unit showed
    "10.0 KB" / "1000 KB".
- Two smoke tests that failed only in a full local run (row-parity highlight, drag on empty space)
  were left over state from earlier tests; fixed on the test side.
- First CI run of all this: macOS now builds with Qt's own packages (universal), whose default media
  backend is FFmpeg; the AVFoundation-only resume never went on there. macOS is pinned to the darwin
  backend (what Homebrew's Qt, used daily, has). Windows/Linux: a drop test inherited Ctrl from the
  ⌘Z before it (offscreen keeps the last modifiers; Ctrl+drop copies there) — test side.
- macOS signing secrets use the same names as the user's other apps (Tauri's): `APPLE_CERTIFICATE`,
  `APPLE_CERTIFICATE_PASSWORD`, `APPLE_ID`, `APPLE_PASSWORD` (app-specific password), `APPLE_TEAM_ID`.
  An App Store Connect API key was set up first and dropped for this, to match them.

## Landing page at files.gitools.net (2026-10-03)

- `site/` is a static page (Korean source, English/Japanese/Chinese in its script, the app's
  language rule) deployed by `.github/workflows/pages.yml` to GitHub Pages; Cloudflare DNS has
  `files` as a DNS-only CNAME to `zidell.github.io`, like `keyscribe.gitools.net`. Pushes that touch
  only `site/` don't build or release (`build.yml` paths-ignore).
- Download buttons read `releases/latest` from the GitHub API in the browser and pick the asset by
  suffix (`-macos.zip`, `-windows-x64.zip`, `-linux-x86_64.AppImage`), so a release needs no
  redeploy and a platform shows up as soon as its asset exists.
- Screenshots (`site/screenshots/`, also in the README) come from `GIFILES_SNAPSHOT` on a generated
  demo folder, with `HOME`, `CFFIXED_USER_HOME` and `GIFILES_CONFIG_DIR` pointed at a scratch
  `Users/demo` so the real session, folders.ini and machine name stay out of them (zsh `PROMPT` set
  to the folder only). In the gallery shot the toolbar still highlights the list button: the snapshot
  switches views without updating the view buttons.
- The page lists the six highlights the user named (Finder usability, colors by extension, Quick
  Look, AI, the selection menu, terminal), each with its screenshot; the list view is the lead
  image. `GIFILES_SNAPSHOT` now also saves `context-menu-window.png`: the window with the context
  menu painted over it and "선택한 항목들로…" open. For the terminal shots the demo `.zshrc` lists the
  folder with `ls -lhgo` (no owner/group) and clears on `chpwd`, so the cd Gifiles types (with the
  scratch path) isn't left on screen; the demo root is named `Macintosh HD`.

## Windows uses the Mac keys too (2026-10-03)

- The user wants Finder's behavior everywhere, so Windows lost its two exceptions from 2026-10-02:
  Return now renames and Ctrl+Down opens there as well (Enter no longer opens, F2 is no longer the
  rename key). Only the modifier mapping differs by platform (⌘ → Ctrl, ⌃ → Ctrl/Alt where ⌘ is
  taken). Keys a user set in config.toml `[shortcuts]` stay as they are; defaults are written
  only as comments, so installed copies pick up the new defaults.

