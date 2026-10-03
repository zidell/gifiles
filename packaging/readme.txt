Gifiles — 설치 사용자를 위한 안내 (readme.txt)
================================================

Gifiles는 아래쪽에 터미널이 붙은 파일 관리자입니다 (macOS · Windows · Linux).
이 파일은 설치된 앱과 함께 배포되며, 환경설정을 찾고 바꾸는 방법을 설명합니다.


1. 설정 파일은 하나입니다: config.toml
--------------------------------------

모든 환경설정(설정 창 ⌘, / Ctrl+, 의 항목과 단축키, 사이드바 즐겨찾기, 확장자별
"항상 이 앱으로 열기")은 사용자 폴더의 config.toml 한 파일에 있습니다.

  macOS    ~/Library/Application Support/Gifiles/config.toml
  Windows  %APPDATA%\Gifiles\config.toml
  Linux    ${XDG_CONFIG_HOME:-~/.config}/gifiles/config.toml

실제로 쓰는 경로는 앱이 알려 줍니다 (위 경로보다 이쪽이 우선):

  macOS    /Applications/Gifiles.app/Contents/MacOS/Gifiles --config-path
  Windows  Gifiles.exe --config-path
  Linux    ./Gifiles-*.AppImage --config-path

환경 변수 GIFILES_CONFIG_DIR를 주면 그 폴더의 config.toml을 씁니다.
설정 창의 "일반" 탭에도 경로와 "Finder에서 보기" 버튼이 있습니다.


2. 파일 형식과 고치는 방법
--------------------------

- 형식은 TOML입니다. '#' 뒤는 주석입니다.
- 각 항목 바로 위에 뜻, 타입, 허용값, 단위, 기본값, 적용 시점이 적혀 있습니다.
  파일만 읽어도 무엇을 어떻게 바꿀지 알 수 있게 되어 있습니다.
- 바꾸고 싶은 항목의 값만 고쳐 저장하세요. 기본값으로 되돌리려면 그 줄을 지우면 됩니다.
- 단축키는 [shortcuts] 표에 메뉴 항목 이름별로 있습니다. 기본값은 주석으로 적혀 있으니
  '#'을 지우고 값을 고치면 됩니다. macOS에서 "Ctrl"은 ⌘, "Meta"는 ⌃(Control)입니다.
- 파일 이름 색은 [file_colors] 표에 있습니다. groups는 { extensions = "zip, 7z", color = "#RRGGBB" }
  항목의 배열이고 folder는 폴더 색입니다. 색은 다크 모드 기준이며 라이트 모드에서는 앱이 더 어둡게
  바꿔 씁니다. 설정 창의 "모양 및 색상" 탭에서도 고칠 수 있습니다.
- 설치 폴더(앱 번들, 실행 파일 옆)에는 설정 파일이 없습니다. 이 readme.txt와 라이선스
  (LICENSE: Gifiles는 MIT, licenses/: 함께 들어 있는 Qt·FFmpeg(LGPL)·libvterm)뿐입니다.


3. 적용 시점
------------

- 앱이 실행 중이면 config.toml을 저장하는 즉시 다시 읽어 반영합니다. 앱을 끌 필요가 없습니다.
- 설명에 "새로 여는 윈도우부터", "새로 여는 터미널부터"라고 적힌 항목만 그때부터 적용됩니다.
- 설정 창에서 바꾼 값도 즉시 같은 파일에 저장됩니다. 앱은 저장할 때 파일 전체를 다시 쓰므로
  항목 설명은 늘 남지만, 사용자가 직접 단 주석은 지워집니다.


4. 잘못된 값
------------

- 문법 오류가 있으면 파일 전체를 무시하고 지금 값을 그대로 씁니다 (파일은 고치지 않습니다).
- 값 하나가 잘못되면(타입·범위·허용값) 그 항목만 무시하고 이전 값을 유지합니다.
- 두 경우 모두 앱이 알림 창으로 줄 번호나 항목 이름, 허용값을 알려 줍니다.
- 앱을 띄우지 않고 검사하려면:  Gifiles --check-config [파일]
  (문제가 없으면 종료 코드 0, 있으면 문제를 출력하고 1)
- 지금 시작하면 실제로 쓸 값을 보려면:  Gifiles --print-config
  (파일의 값, 없는 항목은 기본값. 잘못된 값은 기본값으로 나오고 문제는 오류 출력으로 알립니다.
  실행 중인 앱은 잘못된 값 대신 이전 값을 계속 쓰므로, 고친 뒤에는 알림 창이 사라졌는지도 확인하세요.)


5. 처음 설치했을 때
-------------------

- 처음 실행할 때 모든 항목이 기본값인 config.toml을 설명과 함께 만듭니다.
- 앱을 실행하기 전에 내용을 보려면:  Gifiles --print-default-config
  (이 출력을 위 경로에 저장해 두고 고쳐도 됩니다. 이미 있는 파일은 앱이 덮어쓰지 않습니다.)


6. 그 밖의 저장 위치
--------------------

- 자격 증명(비밀번호·토큰)은 저장하지 않습니다.
- 열린 윈도우와 탭, 창 크기는 앱이 따로 저장하는 상태이며 설정이 아닙니다
  (macOS: ~/Library/Preferences/com.zidell-dev.Gifiles.plist, Windows: 레지스트리
  HKCU\Software\zidell\Gifiles, Linux: ~/.config/zidell/Gifiles.conf).
- 폴더별로 기억하는 보기(보기 방식, 정렬, 열 너비)는 folders.ini에 있습니다
  (macOS: ~/Library/Preferences/zidell/Gifiles/, Windows: %LOCALAPPDATA%\zidell\Gifiles\,
  Linux: ~/.config/zidell/Gifiles/).


7. 자동 업데이트
----------------

- 이 배포판(macOS 앱, Windows zip, Linux AppImage)은 새 버전이 나오면 스스로 받아 둡니다.
  GitHub 릴리스(github.com/zidell/gifiles/releases)의 서명된 업데이트 정보와 SHA-256을 확인한 파일만 씁니다.
- 받아 두면 툴바에 '업데이트'가 나타납니다. 누르면 다시 시작해 설치하고, 누르지 않아도 앱을 끌 때 설치됩니다.
- 끄려면 config.toml의 general.auto_update = false. 파일 메뉴의 '업데이트 확인…'은 언제든 쓸 수 있습니다.
- 앱이 있는 폴더에 쓸 수 없으면(예: 관리자만 쓸 수 있는 곳) 자동으로 설치하지 않고 이유를 알려 줍니다.
  Linux는 AppImage로 실행할 때만 업데이트합니다.
- 버전 확인:  Gifiles --version


명령 요약
---------

  Gifiles --help                   도움말
  Gifiles --version                버전
  Gifiles --config-path            config.toml의 절대 경로 (없으면 만들 위치)
  Gifiles --print-config           지금 시작하면 쓸 설정 (파일의 값 + 나머지 기본값)
  Gifiles --print-default-config   기본값 설정 파일을 설명과 함께 출력
  Gifiles --check-config [파일]    설정 파일 검사


================================================================================
Gifiles — guide for installed copies (English)
================================================================================

Gifiles is a file manager with a terminal attached at the bottom (macOS · Windows · Linux).

1. One settings file: config.toml
   Every setting (the Settings window ⌘, / Ctrl+, , shortcuts, sidebar favorites, the
   "always open with this app" choices, the "With Selected Items…" commands) is in config.toml:
     macOS    ~/Library/Application Support/Gifiles/config.toml
     Windows  %APPDATA%\Gifiles\config.toml
     Linux    ${XDG_CONFIG_HOME:-~/.config}/gifiles/config.toml
   The app prints the path it really uses (prefer it):  Gifiles --config-path
   (macOS: /Applications/Gifiles.app/Contents/MacOS/Gifiles --config-path).
   GIFILES_CONFIG_DIR overrides the folder. There is no settings file in the install folder.

2. Format and editing
   TOML; '#' starts a comment. Above every item the file says what it does, its type, allowed
   values, unit, default and when it applies (comments are written in the app's language).
   Change only the values you need; delete a line to go back to its default. Shortcuts are in
   [shortcuts], keyed by the Korean menu title (the translated title follows in a comment);
   the defaults are commented out — remove the '#' and edit. On macOS "Ctrl" means ⌘ and "Meta"
   means ⌃ (Control). The UI language is general.language ("system", "ko", "en", "ja", "zh_CN").

3. When changes apply
   A running app re-reads config.toml as soon as it is saved; no restart needed, except items
   marked "from new windows/terminals" and the language (restart). Changes made in the Settings
   window are saved to the same file at once; the app rewrites the whole file, so the item
   descriptions stay but comments you added yourself are dropped.

4. Wrong values
   A syntax error: the whole file is ignored and the current values stay (the file is left as is).
   One wrong value (type, range, choice): only that item is ignored and its previous value kept.
   Either way the app shows a notice with the line or item and the allowed values.
   Check without starting the app:  Gifiles --check-config [file]  (exit code 0 = fine, 1 = problems)
   The values a start now would use:  Gifiles --print-config  (the file's values, defaults for the
   rest; a wrong value shows its default and the problem goes to stderr. A running app keeps the
   previous value instead, so after a fix also check that its notice is gone.)

5. First start
   The first start writes config.toml with every item at its default and explained.
   To see it before:  Gifiles --print-default-config  (you may save that output to the path
   above and edit it; the app never overwrites an existing file).

6. Other data
   No credentials are stored. Open windows, tabs and window sizes are app state, not settings
   (macOS: ~/Library/Preferences/com.zidell-dev.Gifiles.plist, Windows: registry
   HKCU\Software\zidell\Gifiles, Linux: ~/.config/zidell/Gifiles.conf). Per-folder views
   (view mode, sort, column widths) are in folders.ini next to that state.

7. Automatic updates
   These packages (macOS app, Windows zip, Linux AppImage) download new versions by themselves,
   using only files whose signed update information and SHA-256 check out (GitHub releases of
   github.com/zidell/gifiles). Once one is downloaded, "Update" appears in the toolbar: click it to
   restart into the new version, or it is installed when the app quits. Turn it off with
   general.auto_update = false; File > Check for Updates… works either way. A copy in a folder the
   app can't write to is not updated (it says why); on Linux only the AppImage updates.
   Version: Gifiles --version


Commands: Gifiles --help | --version | --config-path | --print-config | --print-default-config | --check-config [file]

뷰 이동: Alt+← 사이드바, Alt+→/↑ 파일뷰, Alt+↓ 터미널. 접힌 영역은 펼칩니다.
터미널에서도 뷰 이동이 우선합니다. 설정 → 단축키에서 변경할 수 있습니다.

App keys use Mac-style defaults: Command on macOS, Ctrl on Windows/Linux.
Windows uses the same keys as the Mac: Enter renames, Ctrl+Down opens, Ctrl+Up goes to the parent folder.
Settings > Shortcuts includes file views, Quick Look, terminal, sidebar, search, path entry,
rename editing, context-menu and folder tree keys; keys in different areas can be assigned independently.
