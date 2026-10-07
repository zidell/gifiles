cmake_minimum_required(VERSION 3.21)
# CI sets the version of each release (0.1.<run number>); builds by hand stay 0.1.0.
set(GIFILES_VERSION "0.1.0" CACHE STRING "App version")
project(Gifiles VERSION ${GIFILES_VERSION} LANGUAGES C CXX)
# Automatic updates from the GitHub releases: only the packages CI publishes turn this on.
option(GIFILES_UPDATES "Check for and install new releases (published packages only)" OFF)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(Qt6 6.5 REQUIRED COMPONENTS Widgets Concurrent Multimedia MultimediaWidgets Svg Network)
find_package(Qt6 6.5 QUIET COMPONENTS Pdf PdfWidgets)
qt_standard_project_setup()

qt_add_library(gifiles_core STATIC
    src/App.h src/App.cpp
    src/MainWindow.h src/MainWindow.cpp
    src/BrowserTab.h src/BrowserTab.cpp
    src/FileProxy.h src/FileProxy.cpp
    src/FolderIndex.h src/FolderIndex.cpp src/FolderEvents.h src/FolderEvents.cpp
    src/FolderTree.h src/FolderTree.cpp
    src/ItemDelegate.h src/ItemDelegate.cpp
    src/FileOps.h src/FileOps.cpp
    src/Thumbnails.h src/Thumbnails.cpp
    src/Preview.h src/Preview.cpp
    src/Sidebar.h src/Sidebar.cpp
    src/RecentFolders.h src/RecentFolders.cpp
    src/PathBar.h src/PathBar.cpp
    src/Util.h src/Util.cpp
    src/Theme.h src/Theme.cpp
    src/Permissions.h src/Permissions.cpp
    src/Settings.h src/Settings.cpp
    src/Shortcuts.h src/Shortcuts.cpp
    src/Toml.h src/Toml.cpp
    src/Log.h src/Log.cpp
    src/Pty.h
    src/TerminalWidget.h src/TerminalWidget.cpp
    src/OpenWith.h src/OpenWith.cpp
    src/SystemPreview.h src/SystemPreview.cpp
    src/Updater.h src/Updater.cpp
)
# TweetNaCl (public domain): verifies the Ed25519 signature of the update information.
add_library(tweetnacl STATIC third_party/tweetnacl/tweetnacl.c)
target_include_directories(tweetnacl PUBLIC third_party/tweetnacl)
if(MSVC)
    target_compile_options(tweetnacl PRIVATE /wd4146 /wd4244 /wd4267 /wd4305)
endif()
target_link_libraries(gifiles_core PUBLIC tweetnacl)
target_compile_definitions(gifiles_core PUBLIC GIFILES_VERSION="${PROJECT_VERSION}")
if(WIN32)
    target_sources(gifiles_core PRIVATE src/PtyWin.cpp src/SystemPreviewWin.cpp)
    target_link_libraries(gifiles_core PUBLIC shlwapi ole32) # preview handlers
else()
    target_sources(gifiles_core PRIVATE src/PtyUnix.cpp)
    if(NOT APPLE)
        target_link_libraries(gifiles_core PUBLIC util) # forkpty
    endif()
endif()

# Use the vendored libvterm by default: it includes our wrapped-line resize crash fix.
# A system copy can be selected explicitly with GIFILES_BUNDLED_VTERM=OFF.
option(GIFILES_BUNDLED_VTERM "Build libvterm from source instead of using the system one" ON)
if(NOT GIFILES_BUNDLED_VTERM)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(VTERM IMPORTED_TARGET vterm)
    endif()
endif()
if(VTERM_FOUND AND NOT GIFILES_BUNDLED_VTERM)
    target_link_libraries(gifiles_core PUBLIC PkgConfig::VTERM)
else()
    include(cmake/libvterm.cmake)
    target_link_libraries(gifiles_core PUBLIC vterm_static)
endif()
if(MSVC)
    # Sources are UTF-8 (Korean UI text); MSVC would read them in the system code page.
    target_compile_options(gifiles_core PUBLIC /utf-8)
    target_compile_definitions(gifiles_core PUBLIC NOMINMAX UNICODE _UNICODE)
endif()
if(APPLE)
    enable_language(OBJCXX)
    target_sources(gifiles_core PRIVATE src/MacWindow.h src/MacWindow.mm src/OpenWithMac.mm src/SystemPreviewMac.mm)
    target_link_libraries(gifiles_core PUBLIC "-framework AppKit" "-framework Quartz" "-framework CoreServices")
endif()
# INTERFACE only: on case-insensitive macOS a "-I src" would make <util.h> (forkpty) resolve to src/Util.h.
target_include_directories(gifiles_core INTERFACE src)
target_link_libraries(gifiles_core PUBLIC Qt6::Widgets Qt6::Concurrent Qt6::Multimedia Qt6::MultimediaWidgets Qt6::Svg Qt6::Network)
if(Qt6Pdf_FOUND AND Qt6PdfWidgets_FOUND)
    target_link_libraries(gifiles_core PUBLIC Qt6::Pdf Qt6::PdfWidgets)
    target_compile_definitions(gifiles_core PUBLIC HAVE_QTPDF)
endif()

qt_add_executable(gifiles MACOSX_BUNDLE WIN32 src/main.cpp)
target_link_libraries(gifiles PRIVATE gifiles_core)
# Only the app itself turns updates on (main.cpp); the tests link the same core and never update.
if(GIFILES_UPDATES)
    target_compile_definitions(gifiles PRIVATE GIFILES_UPDATES)
endif()
# Debug log (src/Log.h) into <source>/logs while the app runs on the machine it was built on.
target_compile_definitions(gifiles PRIVATE GIFILES_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")

set_target_properties(gifiles PROPERTIES
    OUTPUT_NAME Gifiles
    MACOSX_BUNDLE_INFO_PLIST ${CMAKE_CURRENT_SOURCE_DIR}/cmake/Info.plist.in
    MACOSX_BUNDLE_BUNDLE_NAME Gifiles
    MACOSX_BUNDLE_GUI_IDENTIFIER dev.zidell.gifiles
    MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION}
    MACOSX_BUNDLE_ICON_FILE gifiles.icns
)

# App icon (packaging/icon/gifiles.svg is the source; .icns/.ico are rendered from it):
# the bundle icon on macOS, the .exe icon on Windows, the window icon elsewhere (Qt resource).
if(APPLE)
    target_sources(gifiles PRIVATE packaging/icon/gifiles.icns packaging/readme.txt
        LICENSE packaging/licenses/THIRD_PARTY_NOTICES.txt packaging/licenses/LGPL-3.0.txt
        packaging/licenses/GPL-3.0.txt packaging/licenses/LGPL-2.1.txt packaging/licenses/OFL-1.1.txt)
    set_source_files_properties(packaging/icon/gifiles.icns PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
    # The installed guide to the settings and the licenses (Windows: next to the .exe; Linux:
    # share/doc in the AppImage).
    set_source_files_properties(packaging/readme.txt LICENSE PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
    set_source_files_properties(packaging/licenses/THIRD_PARTY_NOTICES.txt packaging/licenses/LGPL-3.0.txt
        packaging/licenses/GPL-3.0.txt packaging/licenses/LGPL-2.1.txt packaging/licenses/OFL-1.1.txt
        PROPERTIES MACOSX_PACKAGE_LOCATION Resources/licenses)
elseif(WIN32)
    target_sources(gifiles PRIVATE packaging/windows/gifiles.rc)
endif()
qt_add_resources(gifiles "appicon" PREFIX "/" BASE packaging/icon FILES packaging/icon/gifiles.svg)
# The terminal's default font D2Coding (SIL OFL 1.1, packaging/licenses/OFL-1.1.txt), registered for this
# process only when a terminal is first made (TerminalWidget::bundledFont); never installed system-wide.
qt_add_resources(gifiles_core "fonts" PREFIX "/fonts" BASE packaging/fonts
    FILES packaging/fonts/D2Coding.ttf packaging/fonts/D2CodingBold.ttf)
# UI translations (Korean in the code is the source text). The .qm files are compiled by
# scripts/i18n.sh and committed, so building doesn't need Qt Linguist.
qt_add_resources(gifiles "i18n" PREFIX "/i18n" BASE i18n
    FILES i18n/gifiles_en.qm i18n/gifiles_ja.qm i18n/gifiles_zh_CN.qm)

# macOS keys folder permissions (Desktop, Documents, Full Disk Access...) to the app's code signature.
# The linker's ad-hoc signature changes on every build, so each rebuild looks like a new app and the
# permissions are asked for again. Signing with a real identity keeps them across rebuilds:
#   cmake -DGIFILES_SIGN_IDENTITY="Developer ID Application: ..." (or the SHA-1 from
#   `security find-identity -v -p codesigning`)
set(GIFILES_SIGN_IDENTITY "" CACHE STRING "macOS code signing identity (empty: ad-hoc)")
if(APPLE AND GIFILES_SIGN_IDENTITY)
    add_custom_command(TARGET gifiles POST_BUILD
        COMMAND codesign --force --sign "${GIFILES_SIGN_IDENTITY}" --identifier dev.zidell.gifiles
                $<TARGET_BUNDLE_DIR:gifiles>
        COMMENT "Signing Gifiles.app"
        VERBATIM)
endif()

# Tests, all headless (tests/Headless.h puts them on Qt's offscreen platform): `ctest --test-dir build`.
#   smoke         end to end: one real MainWindow driven with synthetic keys and mouse
#   unit_widgets  one widget at a time (preview, terminal, sidebar, settings window, dialogs)
#   unit_fileops  the file-operation engine and its undo
#   unit_core     the core logic (TOML, settings, quoting, helpers, folder index) and the CLI options
#   unit_update   automatic updates
find_package(Qt6 QUIET COMPONENTS Test)
if(Qt6Test_FOUND)
    enable_testing()
    foreach(name smoke unit_widgets unit_fileops unit_core unit_update)
        qt_add_executable(gifiles_${name} tests/${name}.cpp tests/Headless.h)
        target_link_libraries(gifiles_${name} PRIVATE gifiles_core Qt6::Test)
        target_compile_definitions(gifiles_${name} PRIVATE GIFILES_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
        set_target_properties(gifiles_${name} PROPERTIES WIN32_EXECUTABLE FALSE) # console: test output on stdout
        add_test(NAME ${name} COMMAND gifiles_${name})
        set_tests_properties(${name} PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    endforeach()
    foreach(name smoke unit_widgets)
        qt_add_resources(gifiles_${name} "test_appicon" PREFIX "/" BASE packaging/icon FILES packaging/icon/gifiles.svg)
    endforeach()
    # The CLI tests run the real executable (inside the bundle on macOS).
    target_compile_definitions(gifiles_unit_core PRIVATE GIFILES_APP_EXE="$<TARGET_FILE:gifiles>")
    add_dependencies(gifiles_unit_core gifiles)
endif()
