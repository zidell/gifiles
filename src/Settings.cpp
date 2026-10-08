#include "Settings.h"
#include "RecentFolders.h"
#include "FolderTree.h"
#include "App.h"
#include "Log.h"
#include "Shortcuts.h"
#include "Sidebar.h"
#include "Theme.h"
#include "Toml.h"
#include "Util.h"

#include <QButtonGroup>
#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QFontDatabase>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QRegularExpressionValidator>
#include <QDir>
#include <QProcess>
#include <QUrl>
#include <QFile>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QLocale>
#include <QPointer>
#include <QPushButton>
#include <QScrollArea>
#include <QSaveFile>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>

// ---------------------------------------------------------------------------
// config.toml: the schema. Every item is written with its description, type, allowed values and
// default, so the file alone tells a person (or an agent) what to change and how.

namespace {

enum class Type { Bool, Int, String, Color, Choice, StringList, CommandList, ColorList };

struct Item {
    QString key; // "table/name"
    Type type;
    QVariant def;
    QString what;      // what it does, in the user's words
    QString note = {}; // when it applies, side effects
    int min = 0, max = 0;
    QString unit = {};
    QStringList choices = {};
    bool macOnly = false;
};

struct Table {
    QString name, what;
};

const QList<Table> &tables()
{
    static const QList<Table> t = {
        {QStringLiteral("general"), Gifiles::tr("시작과 윈도우")},
        {QStringLiteral("view"), Gifiles::tr("목록·갤러리·컬럼 보기")},
        {QStringLiteral("appearance"), Gifiles::tr("모양")},
        {QStringLiteral("file_colors"), Gifiles::tr("파일 이름 색 (목록·갤러리·컬럼)")},
        {QStringLiteral("preview"), Gifiles::tr("미리보기 패널과 퀵 뷰어(Space)")},
        {QStringLiteral("terminal"), Gifiles::tr("아래쪽 터미널")},
        {QStringLiteral("sidebar"), Gifiles::tr("사이드바")},
        {QStringLiteral("folder_tree"), Gifiles::tr("폴더 트리 (` 키)")},
        {QStringLiteral("selection_menu"), Gifiles::tr("목록의 컨텍스트 메뉴 \"선택한 항목들로…\"")},
    };
    return t;
}

const QList<Item> &items()
{
    static const QList<Item> list = {
        {Settings::Language, Type::Choice, QStringLiteral("system"),
         Gifiles::tr("화면에 쓸 언어. \"system\" 시스템 언어 따르기 (지원하지 않는 언어면 영어), \"ko\" 한국어, \"en\" English, "
                     "\"ja\" 日本語, \"zh_CN\" 简体中文."),
         Gifiles::tr("앱을 다시 시작하면 적용됩니다."), 0, 0, {},
         {QStringLiteral("system"), QStringLiteral("ko"), QStringLiteral("en"), QStringLiteral("ja"), QStringLiteral("zh_CN")}},
        {Settings::RestoreSession, Type::Bool, true, Gifiles::tr("앱을 시작할 때 지난번 윈도우와 탭을 다시 엽니다.")},
        {Settings::AutoUpdate, Type::Bool, true,
         Gifiles::tr("새 버전이 나오면 자동으로 받아 두었다가, 앱을 끄거나 툴바의 '업데이트'를 누르면 설치합니다.\n"
                     "받은 파일은 서명과 SHA-256을 확인한 뒤에만 씁니다. 6시간마다, 시작하고 1분 뒤에 확인합니다."),
         Gifiles::tr("GitHub 릴리스의 배포판(macOS 앱, Windows zip, Linux AppImage)에서만 동작하고 직접 빌드한 앱은 업데이트하지 않습니다.\n"
                     "false면 확인도 하지 않습니다. 파일 메뉴의 '업데이트 확인…'은 언제든 쓸 수 있습니다.")},
        {Settings::NewWindowHome, Type::Bool, false,
         Gifiles::tr("새로운 윈도우(⌘N / Ctrl+N)를 열 폴더. true: 홈 폴더, false: 지금 보고 있는 폴더.")},
        {Settings::DontAskFullDisk, Type::Bool, false,
         Gifiles::tr("true면 전체 디스크 접근 권한이 없어도 시작할 때 안내 창을 띄우지 않습니다."),
         Gifiles::tr("macOS 전용. 권한 자체는 시스템 설정에서만 줄 수 있습니다."), 0, 0, {}, {}, true},

        {Settings::DefaultMode, Type::Choice, QStringLiteral("list"),
         Gifiles::tr("폴더를 처음 열 때의 보기. \"list\" 목록, \"gallery\" 갤러리, \"columns\" 컬럼."),
         Gifiles::tr("한 번 보기를 바꾼 폴더는 그 폴더의 보기를 따로 기억합니다."), 0, 0, {},
         {QStringLiteral("list"), QStringLiteral("gallery"), QStringLiteral("columns")}},
        {Settings::FoldersFirst, Type::Bool, true, Gifiles::tr("정렬할 때 폴더를 항상 파일보다 위에 둡니다.")},
        {Settings::Stripes, Type::Bool, true, Gifiles::tr("목록 보기에서 줄마다 번갈아 배경색을 칠합니다.")},
        {Settings::ShowHidden, Type::Bool, false,
         Gifiles::tr("숨김 파일(이름이 점으로 시작하는 파일 등)을 보여 줍니다. 보기 메뉴의 '숨김 파일 보기'와 같습니다.")},
        {Settings::IconSize, Type::Int, 96, Gifiles::tr("갤러리 보기의 아이콘 크기. 새로 여는 탭과 폴더에 쓰입니다."), {}, 32, 256,
         QStringLiteral("px")},

        {Settings::FontSize, Type::Int, 0,
         Gifiles::tr("목록·갤러리·컬럼 보기의 글꼴 크기. 0이면 시스템 기본 크기. ⌘+ / ⌘− (Ctrl+ / Ctrl−)로 바꾸고 ⌘0으로 되돌립니다."),
         {}, 0, 32, QStringLiteral("pt")},

        {Settings::ThemeMode, Type::Choice, QStringLiteral("system"),
         Gifiles::tr("색 테마. \"system\" 시스템 설정 따르기, \"light\" 라이트, \"dark\" 다크."), {}, 0, 0, {},
         {QStringLiteral("system"), QStringLiteral("light"), QStringLiteral("dark")}},
        {Settings::BoldNames, Type::Bool, false,
         Gifiles::tr("목록·갤러리·컬럼 보기에서 파일 이름도 굵게 보여 줍니다 (폴더는 늘 굵게).")},
        {Settings::UppercaseNames, Type::Bool, false,
         Gifiles::tr("목록·갤러리·컬럼 보기에서 이름을 항상 대문자로 보여 줍니다. 보이는 모양만 바뀌고 실제 이름은 그대로입니다 (이름 변경 칸에는 실제 이름)."),
         {}},

        {Settings::FileColorSelection, Type::Bool, false,
         Gifiles::tr("선택한 항목의 배경을 그 항목의 색으로 칠합니다 (Mdir의 선택 막대처럼, 글자는 검게). 색이 없는 항목은 회색. false면 강조색 막대 위에 이름 색을 그대로 둡니다.")},
        {Settings::FolderColor, Type::Color, QString(),
         Gifiles::tr("폴더 이름 색 (굵게). \"#RRGGBB\", 다크 모드 기준이고 라이트 모드에서는 더 어둡고 진하게 씁니다.\n"
                        "빈 문자열이면 기본 글자색. Mdir의 폴더 색은 \"#CD6A51\" 입니다.")},
        {Settings::FileColors, Type::ColorList, QVariant(),
         Gifiles::tr("확장자별 파일 이름 색. 한 항목은 한 줄의 { } 이고, extensions에 확장자를 쉼표로 이어 적고\n"
                        "color에 \"#RRGGBB\" 색을 적습니다. 확장자는 대소문자를 가리지 않고, 앞의 점은 빼고 적습니다.\n"
                        "색은 다크 모드 기준입니다. 라이트 모드에서는 앱이 같은 색을 더 어둡고 진하게 바꿔 씁니다.\n"
                        "한 확장자가 여러 항목에 있으면 위의 항목이 이깁니다. 목록에 없는 확장자는 기본 글자색입니다.\n"
                        "예: { extensions = \"psd, ai, sketch\", color = \"#FFB347\" }"),
         Gifiles::tr("주석 처리돼 있으면(줄 앞의 #) 앱의 기본 색(Mdir III의 색: 실행 파일 연두, 배치 파일 노랑, 압축 파일 자홍, 문서 청록 등)을 따라가고,\n"
                        "앱이 기본 색을 바꾸면 그대로 따라갑니다. 바꾸려면 #을 지우고 고치세요. []는 색 없음.")},

        {Settings::QuickLookScale, Type::Int, 100,
         Gifiles::tr("퀵 뷰어(Space)에서 이미지·영상을 원본 크기의 몇 %로 띄울지. 화면의 90%를 넘으면 거기에 맞춥니다.\n"
                        "퀵 뷰어가 열린 채 + / − (⌘+ / ⌘−)로 바꾸거나 창 크기를 끌어 바꾸면 그때의 원본 대비 비율을 기억합니다."),
         {}, 25, 400, QStringLiteral("%")},
        {Settings::QuickLookDocScale, Type::Int, 100,
         Gifiles::tr("퀵 뷰어(Space)에서 텍스트·문서·PDF 등 이미지·영상·소리가 아닌 파일의 글자 크기와 창 크기 (기본 크기의 %).\n"
                        "퀵 뷰어가 열린 채 + / − (⌘+ / ⌘−)로 바꾸면 그 크기를 기억합니다. 화면의 90%를 넘지는 않습니다."),
         {}, 50, 300, QStringLiteral("%")},
        {Settings::QuickLookDocWidth, Type::Int, 0,
         Gifiles::tr("퀵 뷰어에서 텍스트·문서·PDF 창의 너비. 창 크기를 끌어 바꾸면 기억합니다 (글자 크기는 그대로). 0이면 기본 크기 × quick_look_doc_scale."),
         {}, 0, 10000, QStringLiteral("px")},
        {Settings::QuickLookDocHeight, Type::Int, 0, Gifiles::tr("위 창의 높이. 0이면 기본 크기 × quick_look_doc_scale."), {}, 0, 10000,
         QStringLiteral("px")},
        {Settings::QuickLookAudioWidth, Type::Int, 0,
         Gifiles::tr("퀵 뷰어에서 소리 파일 창의 너비. 창 크기를 끌어 바꾸거나 + / −로 바꾸면 기억합니다. 0이면 기본 크기 (520×260)."),
         {}, 0, 10000, QStringLiteral("px")},
        {Settings::QuickLookAudioHeight, Type::Int, 0, Gifiles::tr("위 창의 높이. 0이면 기본 크기."), {}, 0, 10000, QStringLiteral("px")},
        {Settings::MediaVolume, Type::Int, 80, Gifiles::tr("소리·영상 재생 볼륨. 미리보기의 볼륨 막대로 바꾸면 기억합니다."), {}, 0, 100,
         QStringLiteral("%")},
        {Settings::PreviewTextFontSize, Type::Int, 12, Gifiles::tr("코드·설정 파일 미리보기의 글꼴 크기 (고정폭 글꼴)."), {}, 9, 32,
         QStringLiteral("pt")},
        {Settings::PreviewDocFontSize, Type::Int, 14, Gifiles::tr("txt·md 등 글 위주 파일 미리보기의 글꼴 크기."), {}, 9, 32,
         QStringLiteral("pt")},

        {Settings::TermShell, Type::String, QString(),
         Gifiles::tr("터미널에서 실행할 셸의 경로나 이름. 빈 문자열이면 시스템 기본 셸 ($SHELL, Windows는 powershell.exe)."),
         Gifiles::tr("새로 여는 터미널부터 적용됩니다.")},
        {Settings::TermFontFamily, Type::String, QString(),
         Gifiles::tr("터미널 글꼴 이름 (예: \"JetBrains Mono\", \"Menlo\"). 빈 문자열이면 앱에 든 D2Coding (네이버, SIL Open Font License).")},
        {Settings::TermFontSize, Type::Int, 12, Gifiles::tr("터미널 글꼴 크기."), {}, 9, 24, QStringLiteral("pt")},
        {Settings::TermLineHeight, Type::Int, 100, Gifiles::tr("터미널 줄간격 (글꼴 기본 줄 높이의 %). 늘린 만큼 줄 위아래에 고르게 나뉩니다."), {}, 100, 200,
         QStringLiteral("%")},
        {Settings::TermFollowFolder, Type::Bool, true,
         Gifiles::tr("목록에서 폴더를 옮기면 터미널도 그 폴더로 cd 합니다 (셸이 쉬고 있고 입력 중인 명령이 없을 때만).")},
        {Settings::TermSyncBack, Type::Bool, true, Gifiles::tr("터미널에서 cd 하면 목록도 그 폴더로 옮깁니다.")},

        {Settings::SelectionCommands, Type::CommandList, QVariant(),
         Gifiles::tr("목록에서 항목을 고르고 오른쪽 클릭(또는 ⌘↓) → \"선택한 항목들로…\" 메뉴에 나오는 명령들 (위에서부터 순서대로).\n"
                        "한 항목은 한 줄의 { } 이고, 쓸 수 있는 키는 다음과 같습니다:\n"
                        "  label    메뉴에 보일 이름 (필수)\n"
                        "  command  실행할 셸 명령 (필수). macOS·Linux는 터미널의 셸(zsh/bash), Windows는 PowerShell 문법\n"
                        "  key      메뉴가 열려 있을 때 이 글자 하나를 누르면 바로 실행 (A-Z 또는 0-9, 생략 가능)\n"
                        "  terminal true: 아래쪽 터미널을 열고 거기서 실행해 출력을 보여 줌.\n"
                        "           false: 터미널 없이 조용히 실행하고, 실패하면 오류만 알림 (생략하면 true)\n"
                        "  id       기본 명령의 표시 (\"new_folder\", \"zip\", \"ai\"). 기본 명령은 고칠 수는 있어도 지울 수 없어서,\n"
                        "           빠져 있으면 기본값으로 다시 생깁니다. 직접 추가하는 명령에는 쓰지 않습니다.\n"
                        "           \"ai\"에서 claude 대신 codex 등을 쓰려면 그 command를 고치세요.\n"
                        "command 안의 자리표시자는 실행할 때 바뀝니다 (각 경로는 셸에 맞게 따옴표로 감쌈: 공백·한글·따옴표 안전):\n"
                        "  {files}  선택한 항목의 절대 경로들\n"
                        "  {names}  {dir} 기준 경로들 (같은 폴더면 이름)\n"
                        "  {dir}    첫 번째 항목이 있는 폴더 (선택이 없으면 지금 보고 있는 폴더)\n"
                        "  {prompt} 실행할 때 한 줄 입력을 받아 그 글로 바꿈 (메뉴 이름 뒤에 …가 붙음)\n"
                        "여러 항목은 macOS·Linux에서 공백으로, Windows(PowerShell)에서 쉼표로 이어집니다 (배열).\n"
                        "셸 스크립트도 됩니다. 예:\n"
                        "  { label = \"이미지 줄이기\", key = \"R\", terminal = false, command = \"~/bin/shrink.sh {files}\" }"),
         Gifiles::tr("이 줄이 없으면 기본 명령 세 가지(새로운 폴더, 압축 파일 생성, AI로 실행)만 씁니다.\n"
                        "문자열 안의 \" 는 \\\" 로, \\ 는 \\\\ 로 적습니다.")},
        {Settings::Favorites, Type::StringList, QVariant(),
         Gifiles::tr("사이드바 '즐겨찾기'에 보일 폴더 경로들 (순서대로)."),
         Gifiles::tr("이 줄이 없으면 기본 목록(홈, 데스크탑, 문서, 다운로드 등)을 씁니다.")},
        {Settings::RecentFoldersCount, Type::Int, 8,
         Gifiles::tr("사이드바 '최근 폴더'에 보일 폴더 수. 0이면 숨깁니다.\n"
                     "둘러보기만 한 폴더는 들어가지 않고, 무언가를 한 폴더만 들어갑니다: 항목을 복사·이동·만들기·이름 변경·삭제·압축 풀기 했거나,\n"
                     "파일을 열었거나, '선택한 항목들로…' 명령이나 터미널 명령(cd, ls 같은 이동·보기 말고)을 실행한 폴더."),
         {}, 0, 50},

        {Settings::FolderTreeRoots, Type::StringList, QVariant(),
         Gifiles::tr("폴더 트리(파일 목록에서 ` 키)에 보일 폴더들. []이면 드라이브 전체 (macOS·Linux는 \"/\", Windows는 모든 고정 드라이브).\n"
                     "네트워크·FUSE 드라이브는 여기에 직접 적었을 때만 들어갑니다."),
         Gifiles::tr("목록은 백그라운드에서 만들어 캐시에 두고, 오래됐거나 앱에서 폴더를 바꾸면 다시 읽습니다.")},
        {Settings::FolderTreeCase, Type::Bool, false,
         Gifiles::tr("폴더 트리에서 찾을 때 대소문자를 구분합니다. 찾는 칸 오른쪽의 Aa 버튼(⌥C / Alt+C)과 같습니다.")},
        {Settings::FolderTreeExclude, Type::StringList, QVariant(),
         Gifiles::tr("폴더 트리에서 뺄 폴더 (그 안의 모든 폴더도). 이름만 적으면 어디에 있든 그 이름의 폴더를 빼고 (\"node_modules\"),\n"
                     "/ 나 ~ 나 드라이브 문자로 시작하면 그 경로 하나만 뺍니다 (\"~/Library\"). * 와 ? 를 쓸 수 있습니다 (\"*.tmp\")."),
         Gifiles::tr("주석 처리돼 있으면 앱의 기본 목록을 씁니다. macOS에서는 .app 같은 패키지 안은 늘 빠지고, 전체 디스크 접근 권한이 없으면\n"
                     "데스크탑·문서·다운로드는 앱에서 한 번 열어 본 뒤부터 들어갑니다 (macOS가 권한을 묻지 않도록).")},
    };
    return list;
}

const Item *findItem(const QString &key)
{
    for (const Item &i : items())
        if (i.key == key)
            return &i;
    return nullptr;
}

bool itemApplies(const Item &i)
{
#ifdef Q_OS_MACOS
    Q_UNUSED(i);
    return true;
#else
    return !i.macOnly;
#endif
}

QString tableOf(const QString &key) { return key.section(QLatin1Char('/'), 0, 0); }
QString nameOf(const QString &key) { return key.section(QLatin1Char('/'), 1); }

const QString kOpenWith = QStringLiteral("open_with");
const QString kShortcuts = QStringLiteral("shortcuts");

QVariantMap command(const char *id, const QString &label, const char *key, bool terminal, const QString &cmd)
{
    return {{QStringLiteral("id"), QString::fromLatin1(id)}, {QStringLiteral("label"), label},
            {QStringLiteral("key"), QString::fromLatin1(key)}, {QStringLiteral("terminal"), terminal},
            {QStringLiteral("command"), cmd}};
}

// The built-in "선택한 항목들로…" commands. New folder and zip run quietly (the result shows in the
// list); AI runs in the terminal, where its answer appears.
QVariantList defaultSelectionCommands()
{
#ifdef Q_OS_WIN
    return {
        command("new_folder", Gifiles::tr("새로운 폴더"), "N", false,
                Gifiles::tr("Set-Location -LiteralPath {dir}; $d = '새 폴더'; $n = 2; while (Test-Path -LiteralPath $d) { $d = \"새 폴더 $n\"; $n++ }; "
                               "New-Item -ItemType Directory -Path $d | Out-Null; Move-Item -LiteralPath {names} -Destination $d")),
        command("zip", Gifiles::tr("압축 파일 생성"), "Z", false,
                Gifiles::tr("Set-Location -LiteralPath {dir}; $f = '압축 파일.zip'; $n = 2; while (Test-Path -LiteralPath $f) { $f = \"압축 파일 $n.zip\"; $n++ }; "
                               "Compress-Archive -LiteralPath {names} -DestinationPath $f")),
        command("ai", Gifiles::tr("AI로 실행"), "A", true,
                Gifiles::tr("Set-Location -LiteralPath {dir}; claude -p ({prompt} + \"`n`n대상 파일:`n\" + (@({files}) -join \"`n\"))")),
    };
#else
    return {
        command("new_folder", Gifiles::tr("새로운 폴더"), "N", false,
                Gifiles::tr("cd {dir} && d='새 폴더' && n=2 && while [ -e \"$d\" ]; do d=\"새 폴더 $n\"; n=$((n+1)); done && "
                               "mkdir -- \"$d\" && mv -n -- {names} \"$d\"/")),
        command("zip", Gifiles::tr("압축 파일 생성"), "Z", false,
                Gifiles::tr("cd {dir} && f='압축 파일.zip' && n=2 && while [ -e \"$f\" ]; do f=\"압축 파일 $n.zip\"; n=$((n+1)); done && "
                               "zip -r -y -q \"$f\" {names}")),
        command("ai", Gifiles::tr("AI로 실행"), "A", true,
                Gifiles::tr("cd {dir} && claude -p \"$(printf '%s\\n\\n대상 파일:\\n' {prompt}; printf '%s\\n' {files})\"")),
    };
#endif
}

// Built-in commands can't be removed: one missing from the list comes back (at the end).
QVariantList withBuiltins(QVariantList list)
{
    for (const QVariant &d : defaultSelectionCommands()) {
        const QString id = d.toMap().value(QStringLiteral("id")).toString();
        if (std::none_of(list.cbegin(), list.cend(), [&](const QVariant &c) { return c.toMap().value(QStringLiteral("id")).toString() == id; }))
            list << d;
    }
    return list;
}

QVariant defaultOf(const Item &i)
{
    if (i.key == QLatin1String(Settings::FileColors))
        return Settings::defaultFileColors();
    if (i.key == QLatin1String(Settings::Favorites))
        return Sidebar::defaultFavorites();
    if (i.key == QLatin1String(Settings::FolderTreeExclude))
        return FolderTree::defaultExclude();
    if (i.key == QLatin1String(Settings::SelectionCommands))
        return defaultSelectionCommands();
    return i.def;
}

// Checks one value against its item; returns the value in the app's type or an error.
QVariant convert(const Item &i, const QVariant &v, QString &err)
{
    const QString full = tableOf(i.key) + QLatin1Char('.') + nameOf(i.key);
    switch (i.type) {
    case Type::Bool:
        if (v.typeId() == QMetaType::Bool)
            return v;
        err = Gifiles::tr("%1: true 또는 false여야 합니다").arg(full);
        return {};
    case Type::Int:
        if (v.typeId() == QMetaType::LongLong && v.toLongLong() >= i.min && v.toLongLong() <= i.max)
            return int(v.toLongLong());
        err = Gifiles::tr("%1: %2..%3 사이의 정수여야 합니다").arg(full).arg(i.min).arg(i.max);
        return {};
    case Type::String:
        if (v.typeId() == QMetaType::QString)
            return v;
        err = Gifiles::tr("%1: \"문자열\"이어야 합니다").arg(full);
        return {};
    case Type::Color:
        if (v.typeId() == QMetaType::QString && (v.toString().isEmpty() || util::isHexColor(v.toString())))
            return v.toString().toUpper();
        err = Gifiles::tr("%1: \"#RRGGBB\" 형식의 색이거나 빈 문자열이어야 합니다").arg(full);
        return {};
    case Type::Choice:
        if (v.typeId() == QMetaType::QString && i.choices.contains(v.toString()))
            return v;
        err = Gifiles::tr("%1: 허용값은 %2입니다").arg(full, toml::value(i.choices));
        return {};
    case Type::StringList:
        if (v.typeId() == QMetaType::QStringList)
            return v;
        err = Gifiles::tr("%1: [\"문자열\", ...] 배열이어야 합니다").arg(full);
        return {};
    case Type::CommandList: {
        if (v.typeId() == QMetaType::QStringList && v.toStringList().isEmpty())
            return withBuiltins({}); // []
        if (v.typeId() != QMetaType::QVariantList) {
            err = Gifiles::tr("%1: [ { label = \"…\", command = \"…\" }, ... ] 배열이어야 합니다").arg(full);
            return {};
        }
        const QVariantList rows = v.toList();
        QStringList ids, builtinIds;
        for (const QVariant &d : defaultSelectionCommands())
            builtinIds << d.toMap().value(QStringLiteral("id")).toString();
        for (int n = 0; n < rows.size(); ++n) {
            const QVariantMap m = rows[n].toMap();
            const QString at = Gifiles::tr("%1: %2번째 항목").arg(full).arg(n + 1);
            const auto text = [&](const char *f) {
                const QVariant x = m.value(QLatin1String(f));
                return x.typeId() == QMetaType::QString ? x.toString().trimmed() : QString();
            };
            if (text("label").isEmpty() || text("command").isEmpty()) {
                err = Gifiles::tr("%1에 label과 command가 모두 있어야 합니다").arg(at);
                return {};
            }
            const QString key = m.value(QStringLiteral("key")).toString();
            if (m.contains(QStringLiteral("key")) && m.value(QStringLiteral("key")).typeId() != QMetaType::QString) {
                err = Gifiles::tr("%1의 key는 글자 하나(A-Z, 0-9)여야 합니다").arg(at);
                return {};
            }
            if (!key.isEmpty() && (key.size() != 1 || key[0].unicode() > 127 || !key[0].isLetterOrNumber())) {
                err = Gifiles::tr("%1의 key는 글자 하나(A-Z, 0-9)여야 합니다").arg(at);
                return {};
            }
            if (m.contains(QStringLiteral("terminal")) && m.value(QStringLiteral("terminal")).typeId() != QMetaType::Bool) {
                err = Gifiles::tr("%1의 terminal은 true 또는 false여야 합니다").arg(at);
                return {};
            }
            if (const QString id = m.value(QStringLiteral("id")).toString(); !id.isEmpty()) {
                if (!builtinIds.contains(id) || ids.contains(id)) {
                    err = Gifiles::tr("%1의 id는 기본 명령 하나씩에만 씁니다 (%2)").arg(at, builtinIds.join(QStringLiteral(", ")));
                    return {};
                }
                ids << id;
            }
        }
        return withBuiltins(rows);
    }
    case Type::ColorList: {
        if (v.typeId() == QMetaType::QStringList && v.toStringList().isEmpty())
            return QVariantList(); // []
        if (v.typeId() != QMetaType::QVariantList) {
            err = Gifiles::tr("%1: [ { extensions = \"…\", color = \"#RRGGBB\" }, ... ] 배열이어야 합니다").arg(full);
            return {};
        }
        const QVariantList rows = v.toList();
        for (int n = 0; n < rows.size(); ++n) {
            const QVariantMap m = rows[n].toMap();
            const QString at = Gifiles::tr("%1: %2번째 항목").arg(full).arg(n + 1);
            if (m.value(QStringLiteral("extensions")).typeId() != QMetaType::QString ||
                m.value(QStringLiteral("extensions")).toString().trimmed().isEmpty()) {
                err = Gifiles::tr("%1에 extensions(쉼표로 이은 확장자)가 있어야 합니다").arg(at);
                return {};
            }
            if (!util::isHexColor(m.value(QStringLiteral("color")).toString())) {
                err = Gifiles::tr("%1의 color는 \"#RRGGBB\" 형식이어야 합니다").arg(at);
                return {};
            }
        }
        return rows;
    }
    }
    return {};
}

// Reads a parsed file into "table/name" values. Wrong values are reported and left out (the
// caller keeps the previous value); unknown items are reported and ignored.
QMap<QString, QVariant> readDocument(const toml::Document &doc, QStringList &problems)
{
    QMap<QString, QVariant> out;
    for (auto t = doc.cbegin(); t != doc.cend(); ++t) {
        const QString &table = t.key();
        for (auto kv = t.value().cbegin(); kv != t.value().cend(); ++kv) {
            const QString key = table + QLatin1Char('/') + kv.key();
            const QString full = table.isEmpty() ? kv.key() : table + QLatin1Char('.') + kv.key();
            if (table == kOpenWith) {
                if (kv.value().typeId() == QMetaType::QString && !kv.value().toString().isEmpty())
                    out.insert(table + QLatin1Char('/') + kv.key().toLower(), kv.value());
                else
                    problems << Gifiles::tr("%1: 앱 경로 \"문자열\"이어야 합니다").arg(full);
                continue;
            }
            if (table == QLatin1String("ai"))
                continue; // the old AI button's settings: AI is now selection_menu's "ai" command
            if (key == QLatin1String("appearance/native_title_bar"))
                continue; // removed (the unified title bar always; BetterTouchTool works with it since its hit-test fix)
            if (table == kShortcuts) {
                const QString id = Shortcuts::currentId(kv.key()); // a renamed item keeps the user's keys
                const auto &all = Shortcuts::entries();
                if (std::none_of(all.begin(), all.end(), [&](const Shortcuts::Entry &e) { return e.id == id; })) {
                    problems << Gifiles::tr("%1: 이런 메뉴 항목은 없습니다 (무시)").arg(full);
                    continue;
                }
                if (kv.value().typeId() != QMetaType::QStringList) {
                    problems << Gifiles::tr("%1: [\"Ctrl+O\", ...] 배열이어야 합니다").arg(full);
                    continue;
                }
                QStringList bad;
                for (const QString &s : kv.value().toStringList()) {
                    // Qt reads a name it doesn't know as Key_unknown, not as an empty sequence.
                    const QKeySequence seq(s, QKeySequence::PortableText);
                    bool known = !seq.isEmpty();
                    for (int k = 0; known && k < seq.count(); ++k)
                        known = seq[k].key() != Qt::Key_unknown && seq[k].key() != 0;
                    if (!known)
                        bad << s;
                }
                if (!bad.isEmpty()) {
                    problems << Gifiles::tr("%1: 알 수 없는 키 %2").arg(full, toml::value(bad));
                    continue;
                }
                out.insert(table + QLatin1Char('/') + id, kv.value());
                continue;
            }
            const Item *i = findItem(key);
            if (!i) {
                problems << Gifiles::tr("%1: 알 수 없는 항목입니다 (무시)").arg(full);
                continue;
            }
            QString err;
            const QVariant v = convert(*i, kv.value(), err);
            if (v.isValid())
                out.insert(key, v);
            else
                problems << err;
        }
    }
    return out;
}

QStringList commentLines(const QString &text)
{
    QStringList out;
    for (const QString &l : text.split(QLatin1Char('\n')))
        out << (l.isEmpty() ? QStringLiteral("#") : QStringLiteral("# ") + l);
    return out;
}

QString renderValues(const QMap<QString, QVariant> &values, bool defaults)
{
    QStringList o;
    o << Gifiles::tr("# Gifiles 환경설정 — config.toml")
      << QStringLiteral("#")
      << Gifiles::tr("# 형식: TOML. '#' 뒤는 주석입니다. 각 항목 위에 뜻, 타입, 허용값, 기본값을 적어 두었습니다.")
      << Gifiles::tr("# 앱의 설정 창(⌘, / Ctrl+,)에서 바꾼 값도 이 파일에 저장됩니다. 앱이 저장할 때 파일 전체를")
      << Gifiles::tr("# 다시 쓰므로 항목 설명은 늘 남지만, 직접 단 주석은 지워집니다.")
      << Gifiles::tr("# 적용: 앱이 실행 중이면 이 파일을 저장하는 즉시 다시 읽어 반영합니다 (끄고 켤 필요 없음).")
      << Gifiles::tr("#       \"새로 여는 ~부터\"라고 적힌 항목만 그때부터 적용됩니다.")
      << Gifiles::tr("# 잘못된 값: 그 항목만 무시하고 이전 값을 유지하며, 앱이 알림 창으로 알려 줍니다.")
      << Gifiles::tr("#   실행 전에 검사: Gifiles --check-config   (macOS: Gifiles.app/Contents/MacOS/Gifiles)")
      << Gifiles::tr("# 기본값으로 되돌리기: 그 줄을 지우면 됩니다. 파일을 지우면 다음 실행 때 기본값으로 다시 만듭니다.")
      << Gifiles::tr("# 이 파일의 위치: Gifiles --config-path   (환경 변수 GIFILES_CONFIG_DIR로 폴더를 바꿀 수 있음)")
      << Gifiles::tr("# 비밀번호·토큰 같은 자격 증명은 없습니다. 열린 윈도우와 탭, 폴더별 보기 같은 상태는")
      << Gifiles::tr("# 앱이 따로 저장하므로 여기에 없습니다.");
    auto val = [&](const QString &key) { return defaults ? QVariant() : values.value(key); };
    for (const Table &t : tables()) {
        o << QString() << QStringLiteral("[%1]").arg(t.name) << QStringLiteral("# %1").arg(t.what);
        for (const Item &i : items()) {
            if (tableOf(i.key) != t.name || !itemApplies(i))
                continue;
            o << QString();
            o << commentLines(i.what);
            QString kind;
            const QVariant d = defaultOf(i);
            switch (i.type) {
            case Type::Bool: kind = Gifiles::tr("타입: 불리언 (true / false). 기본값: %1.").arg(toml::value(d)); break;
            case Type::Int:
                kind = Gifiles::tr("타입: 정수 %1..%2%3. 기본값: %4.")
                           .arg(i.min).arg(i.max).arg(i.unit.isEmpty() ? QString() : QStringLiteral(" (") + i.unit + QLatin1Char(')'))
                           .arg(d.toInt());
                break;
            case Type::String: kind = Gifiles::tr("타입: 문자열. 기본값: %1.").arg(toml::value(d.toString())); break;
            case Type::Color: kind = Gifiles::tr("타입: 색 \"#RRGGBB\" 또는 \"\". 기본값: %1.").arg(toml::value(d.toString())); break;
            case Type::Choice: {
                QStringList cs;
                for (const QString &c : i.choices)
                    cs << toml::quote(c);
                kind = Gifiles::tr("허용값: %1. 기본값: %2.").arg(cs.join(QStringLiteral(" | ")), toml::quote(d.toString()));
                break;
            }
            case Type::StringList: kind = Gifiles::tr("타입: 문자열 배열. 기본값: %1.").arg(toml::value(d.toStringList())); break;
            case Type::CommandList: kind = Gifiles::tr("타입: { label = \"메뉴 이름\", key = \"글자\", terminal = true, command = \"명령\" }의 배열. 한 항목은 한 줄에."); break;
            case Type::ColorList: kind = Gifiles::tr("타입: { extensions = \"확장자, 확장자\", color = \"#RRGGBB\" }의 배열. 한 항목은 한 줄에."); break;
            }
            o << QStringLiteral("# ") + kind;
            if (!i.note.isEmpty())
                o << commentLines(i.note);
            const QVariant v = val(i.key);
            if (i.type == Type::StringList && !v.isValid())
                o << QStringLiteral("# %1 = %2").arg(nameOf(i.key), toml::value(d.toStringList()));
            else if (i.type == Type::ColorList && !v.isValid()) // commented out: unchanged colors follow the app's defaults
                o << commentLines(QStringLiteral("%1 = %2").arg(nameOf(i.key), toml::value(d)));
            else
                o << QStringLiteral("%1 = %2").arg(nameOf(i.key), toml::value(v.isValid() ? v : d));
        }
    }

    o << QString() << QStringLiteral("[open_with]")
      << Gifiles::tr("# 확장자별로 '항상 이 앱으로 열기'를 고른 앱. 키는 소문자 확장자, 값은 앱 경로")
      << Gifiles::tr("# (macOS .app 번들, Windows .exe, Linux .desktop 파일 이름). 줄을 지우면 시스템 기본 앱으로 엽니다.")
      << Gifiles::tr("# 예: md = \"/Applications/Visual Studio Code.app\"");
    if (!defaults)
        for (auto it = values.cbegin(); it != values.cend(); ++it)
            if (tableOf(it.key()) == kOpenWith)
                o << QStringLiteral("%1 = %2").arg(toml::key(nameOf(it.key())), toml::value(it.value()));

    o << QString() << QStringLiteral("[shortcuts]")
      << Gifiles::tr("# 메뉴 항목의 단축키. 키는 메뉴 항목 이름, 값은 키 문자열 배열이며 []는 단축키 없음입니다.")
      << Gifiles::tr("# 키 문자열은 Qt 표기입니다: macOS에서 \"Ctrl\"은 ⌘, \"Meta\"는 ⌃(Control), \"Alt\"는 ⌥입니다.")
      << Gifiles::tr("#   예: \"Ctrl+Shift+N\", \"Meta+`\", \"Alt+Up\", \"F2\", \"Del\", \"Ctrl+Backspace\"")
      << Gifiles::tr("# 주석 처리된 줄은 기본값입니다. 바꾸려면 '#'을 지우고 값을 고치세요. 같은 키를 두 항목에")
      << Gifiles::tr("# 주면 둘 다 동작하지 않으니 다른 항목에서 그 키를 빼세요 (설정 창의 '재지정'은 자동으로 뺍니다).");
    QString group;
    for (const Shortcuts::Entry &e : Shortcuts::entries()) {
        if (e.group != group) {
            group = e.group;
            o << QString() << Gifiles::tr("# — %1 메뉴 —").arg(Gifiles::tr(group.toUtf8().constData()));
        }
        const QString k = kShortcuts + QLatin1Char('/') + e.id;
        const QString def = toml::value(Shortcuts::toStrings(e.defaults));
        // Keys stay the Korean menu titles in every language; the translated title follows.
        const QString shown = e.title() != e.id ? QStringLiteral("   # ") + e.title() : QString();
        if (!defaults && values.contains(k))
            o << Gifiles::tr("%1 = %2   # 기본값: %3").arg(toml::key(e.id), toml::value(values.value(k)), def) + (shown.isEmpty() ? QString() : QStringLiteral(" · ") + e.title());
        else
            o << QStringLiteral("# %1 = %2").arg(toml::key(e.id), def) + shown;
    }
    return o.join(QLatin1Char('\n')) + QLatin1Char('\n');
}

} // namespace

Settings *Settings::instance()
{
    static Settings *s = new Settings;
    return s;
}

QString Settings::configPath()
{
    QString dir = qEnvironmentVariable("GIFILES_CONFIG_DIR");
    if (dir.isEmpty()) {
#if defined(Q_OS_MACOS)
        dir = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + QStringLiteral("/Gifiles");
#elif defined(Q_OS_WIN)
        dir = qEnvironmentVariable("APPDATA") + QStringLiteral("/Gifiles");
#else
        dir = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + QStringLiteral("/gifiles");
#endif
    }
    return QDir::cleanPath(dir + QStringLiteral("/config.toml"));
}

Settings::Settings()
{
    load(true);
    m_watcher = new QFileSystemWatcher(this);
    auto *debounce = new QTimer(this);
    debounce->setSingleShot(true);
    debounce->setInterval(150);
    connect(debounce, &QTimer::timeout, this, [this] { load(false); });
    connect(m_watcher, &QFileSystemWatcher::fileChanged, debounce, qOverload<>(&QTimer::start));
    connect(m_watcher, &QFileSystemWatcher::directoryChanged, debounce, qOverload<>(&QTimer::start));
    const QString path = configPath();
    QDir().mkpath(QFileInfo(path).absolutePath());
    m_watcher->addPath(QFileInfo(path).absolutePath()); // editors replace the file on save
    if (QFileInfo::exists(path))
        m_watcher->addPath(path);
}

void Settings::load(bool initial)
{
    const QString path = configPath();
    QFile f(path);
    if (!f.exists()) {
        if (initial)
            save(); // a fresh install gets the file with every item explained
        return;
    }
    if (m_watcher && !m_watcher->files().contains(path))
        m_watcher->addPath(path);
    if (!f.open(QIODevice::ReadOnly))
        return;
    const QByteArray bytes = f.readAll();
    if (!initial && bytes == m_written)
        return; // our own save, or a change notice for what was already read
    m_written = bytes;
    toml::Document doc;
    toml::Error err;
    QStringList problems;
    if (!toml::parse(QString::fromUtf8(bytes), doc, err)) {
        // Keep every current value; the file stays as the user wrote it until it's fixed.
        problems << Gifiles::tr("%1번째 줄: %2. 파일을 고칠 때까지 지금 값을 그대로 씁니다.").arg(err.line).arg(err.message);
    } else {
        QMap<QString, QVariant> next = readDocument(doc, problems);
        // A wrong value keeps the old one (the item is still in the file, just not usable).
        const auto inFile = [&doc](const QString &key) {
            const QString table = tableOf(key), name = nameOf(key);
            const auto entries = doc.value(table);
            if (table == kShortcuts || table == kOpenWith) { // shortcut ids may be old names, extensions any case
                for (auto e = entries.cbegin(); e != entries.cend(); ++e)
                    if (table == kShortcuts ? Shortcuts::currentId(e.key()) == name : e.key().toLower() == name)
                        return true;
                return false;
            }
            return findItem(key) && entries.contains(name);
        };
        for (auto it = m_values.cbegin(); it != m_values.cend(); ++it)
            if (!next.contains(it.key()) && inFile(it.key()))
                next.insert(it.key(), it.value());
        QStringList changedKeys;
        QSet<QString> all(m_values.keyBegin(), m_values.keyEnd());
        for (auto it = next.cbegin(); it != next.cend(); ++it)
            all.insert(it.key());
        for (const QString &k : all)
            if (m_values.value(k) != next.value(k))
                changedKeys << k;
        m_values = next;
        if (!initial && !changedKeys.isEmpty())
            Log::write("config", QStringLiteral("reloaded config.toml, changed: %1").arg(changedKeys.join(QStringLiteral(", "))));
        if (!initial)
            for (const QString &k : changedKeys)
                emit changed(k);
    }
    m_problems = problems;
    if (!problems.isEmpty()) {
        qWarning("config.toml: %s", qPrintable(problems.join(QStringLiteral("; "))));
        emit problemsFound(problems);
    }
}

QVariant Settings::value(const QString &key) const
{
    if (const auto it = m_values.constFind(key); it != m_values.cend())
        return key == QLatin1String(SelectionCommands) ? withBuiltins(it.value().toList()) : it.value();
    if (const Item *i = findItem(key))
        return defaultOf(*i);
    return {};
}

bool Settings::contains(const QString &key) const { return m_values.contains(key); }

QStringList Settings::keys(const QString &table) const
{
    QStringList out;
    for (auto it = m_values.cbegin(); it != m_values.cend(); ++it)
        if (tableOf(it.key()) == table)
            out << nameOf(it.key());
    return out;
}

void Settings::setValue(const QString &key, const QVariant &v)
{
    if (m_values.contains(key) && m_values.value(key) == v)
        return;
    const bool same = value(key) == v;
    m_values.insert(key, v);
    save();
    if (!same)
        emit changed(key);
}

void Settings::remove(const QString &key)
{
    if (!m_values.contains(key))
        return;
    const QVariant before = value(key);
    m_values.remove(key);
    save();
    if (value(key) != before)
        emit changed(key);
}

QVariantMap Settings::selectionCommand(const QString &id) const
{
    for (const QVariant &c : value(SelectionCommands).toList())
        if (c.toMap().value(QStringLiteral("id")).toString() == id)
            return c.toMap();
    return {};
}

// Mdir III's hues (its own M.CFG, 3.10, and the user's memory of the screen), grouped by what the files
// are for: programs and installers lime, scripts yellow, archives/disk images magenta, documents teal, source
// code purple, build output blue, pictures olive, video green, sound brown, backups red. Kinds
// Mdir didn't know take the colors it left free: shortcuts cyan (COM's), data/config sky blue, fonts and
// design files rose. Toned down from the pure DOS colors, which glare on a dark background, to one lightness
// (OKLab L 0.78); then the user's tuning: programs and scripts brighter to stand out, pictures and sound dimmer.
QVariantList Settings::defaultFileColors()
{
    auto row = [](const char *exts, const char *color) {
        return QVariantMap{{QStringLiteral("extensions"), QString::fromLatin1(exts)}, {QStringLiteral("color"), QString::fromLatin1(color)}};
    };
    return {
        // programs, and the installers that bring them (apk, dmg, pkg, msi, …)
        row("exe, com, app, appimage, run, apk, ipa, dmg, pkg, msi, deb, rpm", "#00FF09"),
        // scripts and batch files
        row("bat, btm, cmd, sh, bash, zsh, fish, ps1, command", "#F7D800"),
        // shortcuts and links
        row("lnk, url, webloc, desktop", "#35CDDE"),
        // archives and disk images: containers of other files
        row("zip, 7z, rar, tar, gz, tgz, bz2, xz, zst, lz, lzh, lha, arj, arc, ace, ice, zoo, uc2, sqz, hpk, omp, cab, "
            "jar, war, whl, crx, xpi, iso, img, vhd, vhdx, vmdk", "#FA85EE"),
        // documents: text to read, spreadsheets, slides, books
        row("txt, md, markdown, rst, tex, doc, docx, hwp, hwpx, gwp, rtf, odt, pdf, pages, xls, xlsx, ods, numbers, "
            "ppt, pptx, odp, key, epub, mobi, cap, lst, 1st, me", "#35D1C5"),
        // data and configuration
        row("json, jsonl, yaml, yml, toml, xml, ini, cfg, conf, env, plist, csv, tsv, log, lock, db, sqlite, sqlite3, "
            "parquet, pem, crt, cer, pub", "#7BBEF9"),
        // source code
        row("c, cc, cpp, h, hpp, m, mm, cs, java, kt, scala, swift, go, rs, zig, dart, py, ipynb, rb, php, pl, lua, r, "
            "js, mjs, cjs, ts, tsx, jsx, vue, svelte, html, htm, css, scss, sass, sql, hs, clj, ex, exs, gradle, cmake, mk, "
            "pas, asm, bas, cbl", "#C3A5F9"),
        // build output: objects and libraries
        row("o, obj, a, lib, so, dylib, dll, class, pyc, wasm", "#9BB5F9"),
        // pictures
        row("jpg, jpeg, png, gif, webp, heic, heif, avif, bmp, tif, tiff, svg, ico, icns, raw, dng, cr2, cr3, nef, arw, "
            "pcx, lbm", "#7EB206"),
        // video and subtitles
        row("mp4, mov, mkv, avi, webm, m4v, wmv, flv, mpg, mpeg, 3gp, fli, flc, anm, srt, vtt, ass", "#35D87B"),
        // sound
        row("mp3, m4a, aac, flac, wav, aiff, ogg, opus, wma, mid, mod, s3m, voc, ims, stm, rol, bnk, nst, wrk, pcm, okp, "
            "cmf, sop, okm, oka", "#C47623"),
        // backups and temporary files
        row("bak, old, orig, tmp, swp, $$$, olz, crdownload, part", "#FA998F"),
        // fonts, design and 3D files
        row("ttf, otf, ttc, woff, woff2, psd, ai, sketch, fig, xd, afdesign, blend, fbx, stl, 3mf, usdz, glb, gltf", "#FA94B1"),
    };
}

QString Settings::renderDefaults() { return renderValues({}, true); }

QString Settings::render() const { return renderValues(m_values, false); }

QString Settings::renderEffective(QStringList &problems)
{
    QFile f(configPath());
    if (!f.open(QIODevice::ReadOnly))
        return renderValues({}, false);
    toml::Document doc;
    toml::Error err;
    if (!toml::parse(QString::fromUtf8(f.readAll()), doc, err)) {
        problems << Gifiles::tr("%1번째 줄: %2").arg(err.line).arg(err.message);
        return renderValues({}, false);
    }
    return renderValues(readDocument(doc, problems), false);
}


void Settings::save()
{
    const QString path = configPath();
    QDir().mkpath(QFileInfo(path).absolutePath());
    const QByteArray bytes = render().toUtf8();
    QSaveFile f(path);
    if (f.open(QIODevice::WriteOnly) && f.write(bytes) == bytes.size() && f.commit()) {
        m_written = bytes;
        m_saveRetries = 0;
    } else if (m_saveRetries < 10) {
        // Windows: replacing the file fails while another process (the indexer, a virus scanner,
        // our own watcher) still has it open. Try again shortly with what is current then.
        // An edit made outside meanwhile is newer than ours: it wins, read it instead of writing over it.
        ++m_saveRetries;
        QTimer::singleShot(100 * m_saveRetries, this, [this, known = m_written] {
            QFile current(configPath());
            if (current.open(QIODevice::ReadOnly) && current.readAll() != known) {
                m_saveRetries = 0;
                load(false);
                return;
            }
            save();
        });
    } else {
        qWarning("config.toml: cannot write %s: %s", qPrintable(path), qPrintable(f.errorString()));
        m_saveRetries = 0;
    }
    if (m_watcher && !m_watcher->files().contains(path))
        m_watcher->addPath(path);
}

QString Settings::uiLanguage()
{
    QString lang = QStringLiteral("system");
    QFile f(configPath());
    toml::Document doc;
    toml::Error err;
    if (f.open(QIODevice::ReadOnly) && toml::parse(QString::fromUtf8(f.readAll()), doc, err))
        lang = doc.value(QStringLiteral("general")).value(QStringLiteral("language"), lang).toString();
    static const QStringList known = {QStringLiteral("ko"), QStringLiteral("en"), QStringLiteral("ja"), QStringLiteral("zh_CN")};
    if (known.contains(lang))
        return lang;
    for (const QString &l : QLocale::system().uiLanguages()) {
        if (l.startsWith(QLatin1String("ko")))
            return QStringLiteral("ko");
        if (l.startsWith(QLatin1String("ja")))
            return QStringLiteral("ja");
        if (l.startsWith(QLatin1String("zh")))
            return QStringLiteral("zh_CN");
        if (l.startsWith(QLatin1String("en")))
            return QStringLiteral("en");
    }
    return QStringLiteral("en");
}

QStringList Settings::check(const QString &text)
{
    toml::Document doc;
    toml::Error err;
    if (!toml::parse(text, doc, err))
        return {Gifiles::tr("%1번째 줄: %2").arg(err.line).arg(err.message)};
    QStringList problems;
    readDocument(doc, problems);
    return problems;
}

// ---------------------------------------------------------------------------

namespace {

QWidget *page(const QString &title, QFormLayout **form)
{
    auto *w = new QWidget;
    auto *l = new QVBoxLayout(w);
    l->setContentsMargins(28, 22, 28, 22);
    l->setSpacing(14);
    auto *t = new QLabel(title, w);
    t->setObjectName(QStringLiteral("title"));
    l->addWidget(t);
    *form = new QFormLayout;
    (*form)->setHorizontalSpacing(18);
    (*form)->setVerticalSpacing(12);
    (*form)->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
    (*form)->setFieldGrowthPolicy(QFormLayout::FieldsStayAtSizeHint);
    l->addLayout(*form);
    l->addStretch();
    return w;
}

// Keeps a control showing the setting's value, also after config.toml is edited outside the app.
void follow(QWidget *w, const char *key, std::function<void()> update)
{
    update();
    QObject::connect(Settings::instance(), &Settings::changed, w, [w, key, update](const QString &k) {
        if (k == QLatin1String(key)) {
            const QSignalBlocker block(w);
            update();
        }
    });
}

QCheckBox *toggle(const char *key, const QString &text, QWidget *parent)
{
    auto *cb = new QCheckBox(text, parent);
    cb->setObjectName(QStringLiteral("switch"));
    follow(cb, key, [cb, key] { cb->setChecked(Settings::instance()->flag(QString::fromLatin1(key))); });
    QObject::connect(cb, &QCheckBox::toggled, cb, [key](bool on) { Settings::instance()->setValue(QString::fromLatin1(key), on); });
    return cb;
}

QComboBox *choice(const char *key, const QList<QPair<QString, QVariant>> &items, QWidget *parent)
{
    auto *c = new QComboBox(parent);
    for (const auto &[label, v] : items)
        c->addItem(label, v);
    follow(c, key, [c, key] { c->setCurrentIndex(qMax(0, c->findData(Settings::instance()->value(QString::fromLatin1(key))))); });
    QObject::connect(c, &QComboBox::currentIndexChanged, c, [c, key] {
        Settings::instance()->setValue(QString::fromLatin1(key), c->currentData());
    });
    return c;
}

QLabel *hint(const QString &text);

QString keysText(const QList<QKeySequence> &keys)
{
    QStringList parts;
    for (const QKeySequence &k : keys)
        parts << k.toString(QKeySequence::NativeText);
    return parts.isEmpty() ? QStringLiteral("—") : parts.join(QStringLiteral(", "));
}

QString titleOf(const QString &id) { return Gifiles::tr(id.toUtf8().constData()); }

// "재지정" opens this: the key pressed (with its modifiers) shows in the middle and 확인 gives it to
// the action. Every key is captured — Return and Esc too, since they can be shortcuts — so the
// buttons take clicks only. A key another action already uses is pointed out before 확인.
class KeyCaptureDialog : public QDialog {
public:
    std::function<void(const QString &message)> done; // after a change; message: keys taken from others

    KeyCaptureDialog(const QString &id, QWidget *parent) : QDialog(parent), m_id(id)
    {
        setObjectName(QStringLiteral("keyCapture"));
        setWindowTitle(Gifiles::tr("단축키 재지정"));
        setAttribute(Qt::WA_DeleteOnClose);
        setFocusPolicy(Qt::StrongFocus);
        Shortcuts *sc = Shortcuts::instance();
        auto *l = new QVBoxLayout(this);
        l->setContentsMargins(24, 20, 24, 16);
        l->setSpacing(8);
        auto *title = new QLabel(Gifiles::tr("'%1'의 새 단축키").arg(titleOf(id)), this);
        title->setObjectName(QStringLiteral("title"));
        title->setAlignment(Qt::AlignCenter);
        auto *now = new QLabel(Gifiles::tr("지금: %1").arg(keysText(sc->keys(id))), this);
        now->setObjectName(QStringLiteral("secondary"));
        now->setAlignment(Qt::AlignCenter);
        m_key = new QLabel(Gifiles::tr("키를 누르세요"), this);
        m_key->setObjectName(QStringLiteral("keyCaptureText"));
        m_key->setAlignment(Qt::AlignCenter);
        m_key->setMinimumSize(320, 90);
        QFont f = m_key->font();
        f.setPointSizeF(f.pointSizeF() * 2.2);
        m_key->setFont(f);
        m_key->setForegroundRole(QPalette::PlaceholderText);
        m_note = new QLabel(this);
        m_note->setObjectName(QStringLiteral("warning")); // small, red
        m_note->setAlignment(Qt::AlignCenter);
        m_note->setWordWrap(true);
        l->addWidget(title);
        l->addWidget(now);
        l->addWidget(m_key, 1);
        l->addWidget(m_note);

        auto button = [this](const QString &text, const char *name) {
            auto *b = new QPushButton(text, this);
            b->setObjectName(QString::fromLatin1(name));
            b->setFocusPolicy(Qt::NoFocus);
            b->setAutoDefault(false);
            return b;
        };
        auto *cancel = button(Gifiles::tr("취소"), "keyCaptureCancel");
        m_ok = button(Gifiles::tr("확인"), "keyCaptureOk");
        m_ok->setEnabled(false);
        auto *bottom = new QHBoxLayout;
        bottom->addStretch();
        bottom->addWidget(cancel);
        bottom->addWidget(m_ok);
        l->addSpacing(10);
        l->addLayout(bottom);

        connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
        connect(m_ok, &QPushButton::clicked, this, [this, sc] {
            QStringList taken = sc->assign(m_id, m_pressed);
            for (QString &t : taken)
                t = titleOf(t);
            finish(taken.isEmpty() ? QString()
                                   : Gifiles::tr("%1 키를 '%2'에서 뺐습니다.")
                                         .arg(m_pressed.toString(QKeySequence::NativeText), taken.join(QStringLiteral("', '"))));
        });
    }

    // The key a test (or the user) pressed, before 확인.
    QKeySequence pressed() const { return m_pressed; }

protected:
    void showEvent(QShowEvent *e) override
    {
        QDialog::showEvent(e);
        setFocus();
    }

    bool event(QEvent *e) override
    {
        if (e->type() == QEvent::ShortcutOverride) { // no shortcut of the app runs while listening
            e->accept();
            return true;
        }
        if (e->type() == QEvent::KeyPress) { // Tab, Return, Esc included
            capture(static_cast<QKeyEvent *>(e));
            return true;
        }
        return QDialog::event(e);
    }

private:
    void capture(QKeyEvent *e)
    {
        switch (e->key()) {
        case Qt::Key_Shift: case Qt::Key_Control: case Qt::Key_Meta: case Qt::Key_Alt:
        case Qt::Key_AltGr: case Qt::Key_CapsLock: case Qt::Key_unknown: case 0:
            return; // wait for the key itself
        default:
            break;
        }
        m_pressed = QKeySequence(QKeyCombination(e->modifiers() & ~Qt::KeypadModifier, Qt::Key(e->key())));
        m_key->setText(m_pressed.toString(QKeySequence::NativeText));
        m_key->setForegroundRole(QPalette::WindowText);
        m_ok->setEnabled(true);
        QStringList users;
        for (const Shortcuts::Entry &en : Shortcuts::entries())
            if (en.id != m_id && Shortcuts::instance()->conflicts(en.id, m_id)
                && Shortcuts::instance()->keys(en.id).contains(m_pressed))
                users << en.title();
        // Not a refusal: 확인 still assigns it (the user may change the other one later); the key
        // moves over, since a key held by two actions works for neither.
        m_note->setText(users.isEmpty() ? QString()
                                        : Gifiles::tr("'%1'에 이미 할당된 키입니다. 확인하면 이 키로 지정하고 '%1'에서는 뺍니다.")
                                              .arg(users.join(QStringLiteral("', '"))));
    }

    void finish(const QString &message)
    {
        if (done)
            done(message);
        accept();
    }

    QString m_id;
    QKeySequence m_pressed;
    QLabel *m_key;
    QLabel *m_note;
    QPushButton *m_ok;
};

QWidget *shortcutsPage()
{
    auto *w = new QWidget;
    auto *l = new QVBoxLayout(w);
    l->setContentsMargins(28, 22, 28, 22);
    l->setSpacing(10);
    auto *t = new QLabel(Gifiles::tr("단축키"), w);
    t->setObjectName(QStringLiteral("title"));
    auto *head = new QHBoxLayout; // the title, and the small buttons for all shortcuts at its right
    head->addWidget(t);
    head->addStretch();
    l->addLayout(head);
    l->addWidget(hint(Gifiles::tr("재지정을 누르고 새 키를 누른 뒤 확인하면 적용됩니다. "
                                     "다른 기능이 쓰던 키면 그 기능에서는 빠집니다. 굵게 표시된 키는 기본값에서 바꾼 것입니다.")));
    auto *tree = new QTreeWidget(w);
    tree->setObjectName(QStringLiteral("shortcutList"));
    tree->setColumnCount(3);
    tree->setHeaderHidden(true);
    tree->setRootIsDecorated(false);
    tree->setSelectionMode(QAbstractItemView::NoSelection);
    tree->setFocusPolicy(Qt::NoFocus);
    tree->setFrameShape(QFrame::NoFrame);
    tree->header()->setStretchLastSection(false);
    tree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    tree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    tree->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    auto *status = new QLabel(w);
    status->setObjectName(QStringLiteral("secondary"));
    status->setWordWrap(true);

    Shortcuts *sc = Shortcuts::instance();
    QHash<QString, QTreeWidgetItem *> groups;
    auto rows = std::make_shared<QHash<QString, QTreeWidgetItem *>>();
    for (const Shortcuts::Entry &e : sc->entries()) {
        const QString group = e.group.isEmpty() ? Gifiles::tr("기타") : Gifiles::tr(e.group.toUtf8().constData());
        QTreeWidgetItem *g = groups.value(group);
        if (!g) {
            g = new QTreeWidgetItem(tree, {group});
            QFont f = g->font(0);
            f.setBold(true);
            g->setFont(0, f);
            g->setFlags(Qt::ItemIsEnabled);
            g->setFirstColumnSpanned(true);
            groups.insert(group, g);
        }
        auto *item = new QTreeWidgetItem(g, {e.title()});
        item->setFlags(Qt::ItemIsEnabled);
        auto *cell = new QWidget(tree);
        auto *bl = new QHBoxLayout(cell);
        bl->setContentsMargins(8, 2, 0, 2);
        auto *rebind = new QPushButton(Gifiles::tr("재지정"), cell);
        rebind->setObjectName(QStringLiteral("rebind:") + e.id);
        rebind->setFocusPolicy(Qt::NoFocus);
        bl->addWidget(rebind);
        tree->setItemWidget(item, 2, cell);
        const QString id = e.id;
        QObject::connect(rebind, &QPushButton::clicked, rebind, [w, id, status] {
            auto *dlg = new KeyCaptureDialog(id, w->window());
            dlg->done = [status](const QString &message) { status->setText(message); };
            dlg->open();
        });
        rows->insert(id, item);
    }
    tree->expandAll();
    auto refresh = [sc, rows] {
        for (auto it = rows->cbegin(); it != rows->cend(); ++it) {
            it.value()->setText(1, keysText(sc->keys(it.key())));
            QFont f = it.value()->font(1);
            f.setBold(sc->isCustom(it.key())); // changed from the built-in keys
            it.value()->setFont(1, f);
        }
    };
    refresh();
    QObject::connect(sc, &Shortcuts::changed, tree, refresh);
    l->addWidget(tree, 1);
    l->addWidget(status);
    // For every shortcut at once, each asked once more. Small and top right: at the bottom center
    // they were taken for the window's OK button.
    auto *resetAll = new QPushButton(Gifiles::tr("초기화"), w);
    resetAll->setObjectName(QStringLiteral("shortcutsReset"));
    resetAll->setProperty("small", true);
    resetAll->setToolTip(Gifiles::tr("모든 단축키를 기본값으로"));
    auto *clearAll = new QPushButton(Gifiles::tr("전부 제거"), w);
    clearAll->setObjectName(QStringLiteral("shortcutsClear"));
    clearAll->setProperty("small", true);
    clearAll->setToolTip(Gifiles::tr("모든 메뉴 항목의 단축키를 지움"));
    head->addWidget(resetAll);
    head->addWidget(clearAll);
    auto ask = [w](const QString &q) {
        return QMessageBox::question(w->window(), Gifiles::tr("단축키"), q, QMessageBox::Yes | QMessageBox::Cancel,
                                     QMessageBox::Cancel) == QMessageBox::Yes;
    };
    QObject::connect(resetAll, &QPushButton::clicked, w, [sc, ask, status] {
        if (ask(Gifiles::tr("모든 단축키를 기본값으로 되돌릴까요? 직접 바꾼 키는 사라집니다."))) {
            sc->resetAll();
            status->clear();
        }
    });
    QObject::connect(clearAll, &QPushButton::clicked, w, [sc, ask, status] {
        if (ask(Gifiles::tr("모든 메뉴 항목의 단축키를 지울까요? 초기화로 기본값을 되돌릴 수 있습니다."))) {
            sc->clearAll();
            status->clear();
        }
    });
    return w;
}

// One "선택한 항목들로…" command: name, its letter in the menu, the command, and whether it runs in
// the terminal (shown) or quietly.
class CommandEditDialog : public QDialog {
public:
    CommandEditDialog(const QVariantMap &command, bool isNew, QWidget *parent) : QDialog(parent), m_command(command)
    {
        setObjectName(QStringLiteral("commandEdit"));
        setWindowTitle(isNew ? Gifiles::tr("명령 추가") : Gifiles::tr("명령 편집"));
        setAttribute(Qt::WA_DeleteOnClose);
        auto *l = new QVBoxLayout(this);
        l->setContentsMargins(22, 18, 22, 14);
        l->setSpacing(10);
        auto *form = new QFormLayout;
        form->setHorizontalSpacing(14);
        form->setVerticalSpacing(10);
        form->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
        m_label = new QLineEdit(command.value(QStringLiteral("label")).toString(), this);
        m_label->setObjectName(QStringLiteral("commandLabel"));
        m_key = new QLineEdit(command.value(QStringLiteral("key")).toString(), this);
        m_key->setObjectName(QStringLiteral("commandKey"));
        m_key->setMaxLength(1);
        m_key->setFixedWidth(44);
        m_key->setAlignment(Qt::AlignCenter);
        m_key->setValidator(new QRegularExpressionValidator(QRegularExpression(QStringLiteral("[A-Za-z0-9]?")), m_key));
        auto *keyRow = new QHBoxLayout;
        keyRow->addWidget(m_key);
        keyRow->addWidget(hint(Gifiles::tr("메뉴가 열려 있을 때 이 글자를 누르면 바로 실행 (A-Z, 0-9)")), 1);
        m_text = new QPlainTextEdit(command.value(QStringLiteral("command")).toString(), this);
        m_text->setObjectName(QStringLiteral("commandText"));
        m_text->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
        m_text->setMinimumSize(440, 110);
        m_text->setMaximumHeight(160);
        m_text->setTabChangesFocus(true);
        m_terminal = new QCheckBox(Gifiles::tr("터미널 열기"), this);
        m_terminal->setObjectName(QStringLiteral("commandTerminal"));
        m_terminal->setChecked(command.value(QStringLiteral("terminal"), true).toBool());
        form->addRow(Gifiles::tr("이름:"), m_label);
        form->addRow(Gifiles::tr("단축키(글자):"), keyRow);
        form->addRow(Gifiles::tr("명령어:"), m_text);
        form->addRow(QString(), hint(Gifiles::tr("{files} 선택한 항목의 경로 · {names} 폴더 기준 이름 · {dir} 그 폴더 · "
                                                    "{prompt} 실행할 때 한 줄 입력받기. 경로는 셸에 맞게 따옴표로 감싸 전달됩니다 "
                                                    "(공백·한글·따옴표 안전).")));
        auto *termBox = new QVBoxLayout;
        termBox->setSpacing(2);
        termBox->addWidget(m_terminal);
        termBox->addWidget(hint(Gifiles::tr("끄면 터미널 없이 조용히 실행하고, 실패했을 때만 알립니다.")));
        form->addRow(QString(), termBox);
        l->addLayout(form);
        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
        buttons->button(QDialogButtonBox::Ok)->setText(Gifiles::tr("확인"));
        buttons->button(QDialogButtonBox::Cancel)->setText(Gifiles::tr("취소"));
        l->addWidget(buttons);
        connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
        auto valid = [this, buttons] {
            buttons->button(QDialogButtonBox::Ok)->setEnabled(!m_label->text().trimmed().isEmpty() &&
                                                              !m_text->toPlainText().trimmed().isEmpty());
        };
        valid();
        connect(m_label, &QLineEdit::textChanged, this, valid);
        connect(m_text, &QPlainTextEdit::textChanged, this, valid);
        m_label->setFocus();
    }

    QVariantMap command() const
    {
        QVariantMap m = m_command; // keeps the id of a built-in command
        m.insert(QStringLiteral("label"), m_label->text().trimmed());
        m.insert(QStringLiteral("key"), m_key->text().toUpper());
        m.insert(QStringLiteral("terminal"), m_terminal->isChecked());
        m.insert(QStringLiteral("command"), m_text->toPlainText().trimmed());
        return m;
    }

private:
    QVariantMap m_command;
    QLineEdit *m_label;
    QLineEdit *m_key;
    QPlainTextEdit *m_text;
    QCheckBox *m_terminal;
};

// "선택한 항목들로…" commands: the list, and buttons under it; 추가 and 편집 open CommandEditDialog.
QWidget *selectionMenuPage()
{
    auto *w = new QWidget;
    auto *l = new QVBoxLayout(w);
    l->setContentsMargins(28, 22, 28, 22);
    l->setSpacing(10);
    auto *t = new QLabel(Gifiles::tr("선택 항목 메뉴"), w);
    t->setObjectName(QStringLiteral("title"));
    l->addWidget(t);
    l->addWidget(hint(Gifiles::tr("목록에서 항목을 고르고 오른쪽 클릭(또는 ⌘↓) → \"선택한 항목들로…\"에 나오는 명령입니다. "
                                     "셸 스크립트도 등록할 수 있습니다. 기본 명령 셋은 고칠 수 있지만 지울 수는 없습니다.")));
    auto *list = new QListWidget(w);
    list->setObjectName(QStringLiteral("commandList"));
    list->setSpacing(2);
    l->addWidget(list, 1);
    auto *shown = new QLabel(w); // the chosen command, as it will run
    shown->setObjectName(QStringLiteral("secondary"));
    shown->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    shown->setWordWrap(true);
    shown->setTextFormat(Qt::PlainText);
    shown->setTextInteractionFlags(Qt::TextSelectableByMouse);
    l->addWidget(shown);
    auto *buttons = new QHBoxLayout;
    auto button = [w, buttons](const QString &text, const char *name) {
        auto *b = new QPushButton(text, w);
        b->setObjectName(QString::fromLatin1(name));
        buttons->addWidget(b);
        return b;
    };
    auto *addBtn = button(Gifiles::tr("추가"), "commandAdd");
    auto *editBtn = button(Gifiles::tr("편집"), "commandEdit");
    auto *removeBtn = button(Gifiles::tr("삭제"), "commandRemove");
    auto *upBtn = button(Gifiles::tr("위로"), "commandUp");
    auto *downBtn = button(Gifiles::tr("아래로"), "commandDown");
    buttons->addStretch();
    auto *defaults = button(Gifiles::tr("기본값으로"), "commandDefaults");
    l->addLayout(buttons);

    auto commands = [] { return Settings::instance()->value(Settings::SelectionCommands).toList(); };
    auto store = [](const QVariantList &v) { Settings::instance()->setValue(Settings::SelectionCommands, v); };
    auto sync = [list, shown, editBtn, removeBtn, upBtn, downBtn, commands] {
        const int r = list->currentRow();
        const QVariantList all = commands();
        const QVariantMap cur = all.value(r).toMap();
        shown->setText(cur.value(QStringLiteral("command")).toString());
        editBtn->setEnabled(r >= 0);
        removeBtn->setEnabled(r >= 0 && cur.value(QStringLiteral("id")).toString().isEmpty()); // built-ins stay
        upBtn->setEnabled(r > 0);
        downBtn->setEnabled(r >= 0 && r < all.size() - 1);
    };
    auto refresh = [list, commands, sync](int select) {
        {
            const QSignalBlocker b(list);
            list->clear();
            for (const QVariant &c : commands()) {
                const QVariantMap m = c.toMap();
                QString text = m.value(QStringLiteral("label")).toString();
                if (!m.value(QStringLiteral("key")).toString().isEmpty())
                    text += QStringLiteral(" (%1)").arg(m.value(QStringLiteral("key")).toString().toUpper());
                QStringList tags;
                if (!m.value(QStringLiteral("terminal"), true).toBool())
                    tags << Gifiles::tr("터미널 없이");
                if (!m.value(QStringLiteral("id")).toString().isEmpty())
                    tags << Gifiles::tr("기본");
                if (!tags.isEmpty())
                    text += QStringLiteral("   · ") + tags.join(QStringLiteral(" · "));
                auto *item = new QListWidgetItem(text, list);
                item->setSizeHint(QSize(0, 34));
            }
            list->setCurrentRow(qBound(-1, select, list->count() - 1));
        }
        sync();
    };
    refresh(0);
    QObject::connect(list, &QListWidget::currentRowChanged, w, sync);
    auto edit = [w, list, commands, store, refresh](int row) {
        const QVariantList all = commands();
        const bool isNew = row < 0;
#ifdef Q_OS_WIN
        const QString example = QStringLiteral("Write-Output {files}");
#else
        const QString example = QStringLiteral("echo {files}");
#endif
        const QVariantMap start = isNew ? QVariantMap{{QStringLiteral("label"), Gifiles::tr("새 명령")},
                                                      {QStringLiteral("terminal"), true},
                                                      {QStringLiteral("command"), example}}
                                        : all.value(row).toMap();
        auto *dlg = new CommandEditDialog(start, isNew, w->window());
        QObject::connect(dlg, &QDialog::accepted, w, [dlg, row, commands, store, refresh] {
            QVariantList all = commands();
            const int at = row < 0 || row >= all.size() ? int(all.size()) : row;
            if (at == all.size())
                all << dlg->command();
            else
                all[at] = dlg->command();
            store(all);
            refresh(at);
        });
        dlg->open();
        Q_UNUSED(list);
    };
    QObject::connect(addBtn, &QPushButton::clicked, w, [edit] { edit(-1); });
    QObject::connect(editBtn, &QPushButton::clicked, w, [edit, list] { edit(list->currentRow()); });
    QObject::connect(list, &QListWidget::itemActivated, w, [edit, list] { edit(list->currentRow()); });
    QObject::connect(removeBtn, &QPushButton::clicked, w, [list, commands, store, refresh] {
        QVariantList all = commands();
        const int r = list->currentRow();
        if (r < 0 || r >= all.size() || !all[r].toMap().value(QStringLiteral("id")).toString().isEmpty())
            return;
        all.removeAt(r);
        store(all);
        refresh(r);
    });
    auto move = [list, commands, store, refresh](int by) {
        QVariantList all = commands();
        const int r = list->currentRow(), to = r + by;
        if (r < 0 || to < 0 || to >= all.size())
            return;
        all.move(r, to);
        store(all);
        refresh(to);
    };
    QObject::connect(upBtn, &QPushButton::clicked, w, [move] { move(-1); });
    QObject::connect(downBtn, &QPushButton::clicked, w, [move] { move(1); });
    QObject::connect(defaults, &QPushButton::clicked, w, [w, refresh] {
        if (QMessageBox::question(w->window(), Gifiles::tr("선택 항목 메뉴"),
                                  Gifiles::tr("직접 추가한 명령을 지우고 기본 명령 셋으로 되돌릴까요?"),
                                  QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Yes)
            return;
        Settings::instance()->remove(Settings::SelectionCommands);
        refresh(0);
    });
    QObject::connect(Settings::instance(), &Settings::changed, list, [list, refresh](const QString &k) {
        if (k == QLatin1String(Settings::SelectionCommands) || k.isEmpty())
            refresh(list->currentRow());
    });
    return w;
}

// 색상: the folder color and the file colors by extension. Each row is the extensions (comma separated,
// drawn in their color as it shows in the current theme), the color as "#RRGGBB" with a swatch that opens
// the color picker, and 삭제. Colors are given for dark mode; light mode darkens them (Theme::lightModeColor).
// "모양 및 색상": the appearance switches (`look`, a form) on top, the colors below.
QWidget *appearancePage(const std::function<QWidget *(QWidget *page)> &look)
{
    auto *w = new QWidget;
    auto *l = new QVBoxLayout(w);
    l->setContentsMargins(28, 22, 28, 22);
    l->setSpacing(10);
    auto *t = new QLabel(Gifiles::tr("모양 및 색상"), w);
    t->setObjectName(QStringLiteral("title"));
    l->addWidget(t);
    l->addWidget(look(w));
    l->addSpacing(8);
    auto *section = new QLabel(Gifiles::tr("색상"), w);
    QFont sf = section->font();
    sf.setBold(true);
    section->setFont(sf);
    // 초기화 sits small at the section's right (it resets the colors only, not the switches above);
    // at the bottom center it was taken for the window's OK button.
    auto *reset = new QPushButton(Gifiles::tr("초기화"), w);
    reset->setObjectName(QStringLiteral("colorsReset"));
    reset->setProperty("small", true);
    reset->setToolTip(Gifiles::tr("폴더와 모든 확장자 색을 기본값으로"));
    auto *sectionRow = new QHBoxLayout;
    sectionRow->addWidget(section);
    sectionRow->addStretch();
    sectionRow->addWidget(reset);
    l->addLayout(sectionRow);
    QLabel *about = hint(Gifiles::tr("목록·갤러리·컬럼에서 파일 이름을 확장자별로 칠할 색입니다. 왼쪽에 확장자를 쉼표로 이어 적고 "
                                     "오른쪽에 색을 고릅니다. 색은 다크 모드 기준이고, 라이트 모드에서는 더 어둡고 진하게 바뀝니다. "
                                     "한 확장자가 여러 줄에 있으면 위의 줄이 이깁니다."));
    l->addWidget(about);

    auto *scroll = new QScrollArea(w);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto *body = new QWidget(scroll);
    body->setObjectName(QStringLiteral("colorRows"));
    auto *rows = new QVBoxLayout(body);
    rows->setContentsMargins(0, 0, 8, 0);
    rows->setSpacing(6);
    scroll->setWidget(body);
    l->addWidget(scroll, 1);

    auto shown = [](const QString &hex) {
        const QColor c(hex);
        return Theme::colors().dark ? c : Theme::lightModeColor(c);
    };
    // A color: swatch (opens the picker) and the hex text; done() after a valid change.
    // An empty field (only where `plain` is given) means the normal text color. While the picker is open,
    // preview(color) shows each color it points at in the file views; preview({}) ends that.
    auto colorField = [w](QWidget *parent, QHBoxLayout *into, const QString &hex, const QString &name,
                          std::function<void()> done, std::function<void(const QColor &)> preview,
                          const QString &plain = {}) {
        auto *swatch = new QPushButton(parent);
        swatch->setObjectName(QStringLiteral("colorSwatch"));
        swatch->setFixedSize(30, 22);
        swatch->setFocusPolicy(Qt::NoFocus);
        auto *edit = new QLineEdit(hex, parent);
        edit->setObjectName(name);
        edit->setFixedWidth(84);
        edit->setValidator(new QRegularExpressionValidator(QRegularExpression(QStringLiteral("#?[0-9A-Fa-f]{0,6}")), edit));
        if (!plain.isEmpty())
            edit->setPlaceholderText(plain);
        auto paint = [swatch, edit](const QString &shownHex = {}) {
            const QString c = !shownHex.isEmpty() ? shownHex : edit->text().isEmpty() ? Theme::colors().text.name() : edit->text();
            if (util::isHexColor(c))
                swatch->setStyleSheet(QStringLiteral("QPushButton { background: %1; border: 1px solid %2; border-radius: 4px; }")
                                          .arg(c, Theme::colors().separator.name()));
        };
        paint();
        QObject::connect(edit, &QLineEdit::editingFinished, edit, [edit, paint, done, plain] {
            QString v = edit->text().trimmed().toUpper();
            if (v.isEmpty() && !plain.isEmpty()) {
                paint();
                done();
                return;
            }
            if (!v.startsWith(QLatin1Char('#')))
                v.prepend(QLatin1Char('#'));
            if (!util::isHexColor(v))
                return; // the stored color stays; a refresh shows it again
            edit->setText(v);
            paint();
            done();
        });
        QObject::connect(swatch, &QPushButton::clicked, swatch, [w, edit, paint, done, preview] {
            const QColor start = edit->text().isEmpty() ? Theme::colors().text : QColor(edit->text());
            auto *dlg = new QColorDialog(start, w->window());
            dlg->setObjectName(QStringLiteral("colorPicker"));
            dlg->setWindowTitle(Gifiles::tr("색 고르기"));
            dlg->setAttribute(Qt::WA_DeleteOnClose);
            // Every color pointed at shows at once in the lists, to judge it there; 취소 puts the old one back.
            QObject::connect(dlg, &QColorDialog::currentColorChanged, edit, [paint, preview](const QColor &c) {
                if (!c.isValid())
                    return;
                paint(c.name(QColor::HexRgb));
                preview(c);
            });
            QObject::connect(dlg, &QColorDialog::colorSelected, edit, [edit, paint, done](const QColor &c) {
                edit->setText(c.name(QColor::HexRgb).toUpper());
                paint();
                done();
            });
            QObject::connect(dlg, &QDialog::finished, edit, [paint, preview] {
                preview({});
                paint();
            });
            dlg->open();
        });
        into->addWidget(swatch);
        into->addWidget(edit);
        return edit;
    };

    auto own = std::make_shared<bool>(false); // the page's own change: don't rebuild the rows under the cursor
    auto rebuild = std::make_shared<std::function<void()>>();
    // The rows as they are now, back into config.toml (rows without extensions aren't saved yet).
    auto store = [body, own] {
        QVariantList out;
        for (QWidget *row : body->findChildren<QWidget *>(QStringLiteral("colorRow"), Qt::FindDirectChildrenOnly)) {
            const auto *ext = row->findChild<QLineEdit *>(QStringLiteral("colorExt"));
            const auto *hex = row->findChild<QLineEdit *>(QStringLiteral("colorHex"));
            const QStringList exts = util::extensionList(ext->text());
            if (!exts.isEmpty() && util::isHexColor(hex->text()))
                out << QVariantMap{{QStringLiteral("extensions"), exts.join(QStringLiteral(", "))},
                                   {QStringLiteral("color"), hex->text()}};
        }
        *own = true;
        Settings::instance()->setValue(Settings::FileColors, out);
        *own = false;
    };
    auto tint = [shown](QLineEdit *ext, const QString &hex) {
        ext->setStyleSheet(QStringLiteral("QLineEdit { color: %1; }").arg(shown(hex).name()));
    };
    auto addRow = [body, rows, colorField, store, tint](const QString &exts, const QString &hex) {
        auto *row = new QWidget(body);
        row->setObjectName(QStringLiteral("colorRow"));
        auto *h = new QHBoxLayout(row);
        h->setContentsMargins(0, 0, 0, 0);
        h->setSpacing(8);
        auto *ext = new QLineEdit(exts, row);
        ext->setObjectName(QStringLiteral("colorExt"));
        ext->setPlaceholderText(Gifiles::tr("확장자, 확장자 (예: zip, 7z)"));
        ext->setCursorPosition(0);
        h->addWidget(ext, 1);
        colorField(row, h, hex, QStringLiteral("colorHex"), [store, tint, ext, row] {
            tint(ext, row->findChild<QLineEdit *>(QStringLiteral("colorHex"))->text());
            store();
        }, [ext, tint, row](const QColor &c) {
            Theme::previewFileColor(util::extensionList(ext->text()), c);
            tint(ext, c.isValid() ? c.name() : row->findChild<QLineEdit *>(QStringLiteral("colorHex"))->text());
        });
        tint(ext, hex);
        QObject::connect(ext, &QLineEdit::editingFinished, ext, [store] { store(); });
        auto *remove = new QPushButton(Gifiles::tr("삭제"), row);
        remove->setObjectName(QStringLiteral("colorRemove"));
        remove->setFocusPolicy(Qt::NoFocus);
        QObject::connect(remove, &QPushButton::clicked, row, [row, store] {
            row->setParent(nullptr); // out of the rows before they're stored
            row->deleteLater();
            store();
        });
        h->addWidget(remove);
        rows->insertWidget(rows->count() - 1, row); // above the stretch
        return ext;
    };

    // 폴더 above the rows
    auto *folderRow = new QWidget(w);
    auto *fh = new QHBoxLayout(folderRow);
    fh->setContentsMargins(0, 0, 0, 4);
    fh->setSpacing(8);
    auto *folderLabel = new QLabel(Gifiles::tr("폴더"), folderRow);
    fh->addWidget(folderLabel, 1);
    auto *folderHex = colorField(folderRow, fh, Settings::instance()->value(Settings::FolderColor).toString(),
                                 QStringLiteral("folderColorHex"), [own, folderRow] {
                                     *own = true;
                                     Settings::instance()->setValue(Settings::FolderColor,
                                                                    folderRow->findChild<QLineEdit *>(QStringLiteral("folderColorHex"))->text());
                                     *own = false;
                                 }, [](const QColor &c) { Theme::previewFolderColor(c); }, Gifiles::tr("기본"));
    auto tintFolder = [folderLabel, folderHex, shown] {
        const QString hex = folderHex->text();
        folderLabel->setStyleSheet(QStringLiteral("QLabel { color: %1; font-weight: bold; }")
                                       .arg(util::isHexColor(hex) ? shown(hex).name() : Theme::colors().text.name()));
    };
    tintFolder();
    QObject::connect(folderHex, &QLineEdit::textChanged, folderLabel, tintFolder);
    const int afterAbout = l->indexOf(about) + 1;
    l->insertWidget(afterAbout, folderRow);
    auto *selection = toggle(Settings::FileColorSelection, Gifiles::tr("선택한 항목의 배경도 이 색으로"), w);
    l->insertWidget(afterAbout + 1, selection);

    rows->addStretch();
    *rebuild = [body, addRow] {
        for (QWidget *row : body->findChildren<QWidget *>(QStringLiteral("colorRow"), Qt::FindDirectChildrenOnly))
            delete row;
        for (const QVariant &v : Settings::instance()->value(Settings::FileColors).toList()) {
            const QVariantMap m = v.toMap();
            addRow(m.value(QStringLiteral("extensions")).toString(), m.value(QStringLiteral("color")).toString());
        }
    };
    (*rebuild)();

    auto *buttons = new QHBoxLayout;
    auto *add = new QPushButton(Gifiles::tr("추가"), w);
    add->setObjectName(QStringLiteral("colorAdd"));
    QObject::connect(add, &QPushButton::clicked, w, [addRow, scroll] {
        QLineEdit *ext = addRow(QString(), QStringLiteral("#FFFFFF"));
        QTimer::singleShot(0, ext, [ext, scroll] {
            scroll->ensureWidgetVisible(ext);
            ext->setFocus();
        });
    });
    buttons->addWidget(add);
    buttons->addStretch();
    l->addLayout(buttons);
    // 초기화: back to the built-in colors, the folder's too, asked once more.
    QObject::connect(reset, &QPushButton::clicked, w, [w] {
        if (QMessageBox::question(w->window(), Gifiles::tr("색상"),
                                  Gifiles::tr("폴더와 확장자 색을 모두 기본값으로 되돌릴까요? 직접 바꾼 색은 사라집니다."),
                                  QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Yes)
            return;
        Settings::instance()->remove(Settings::FileColors);
        Settings::instance()->remove(Settings::FolderColor);
    });

    // config.toml edited outside, 초기화, or the theme switched (the preview tints follow it)
    QObject::connect(Settings::instance(), &Settings::changed, body, [own, rebuild, folderHex](const QString &k) {
        if (*own)
            return;
        if (k == QLatin1String(Settings::FileColors) || k.isEmpty())
            (*rebuild)();
        if (k == QLatin1String(Settings::FolderColor) || k.isEmpty())
            folderHex->setText(Settings::instance()->value(Settings::FolderColor).toString());
    });
    QObject::connect(Theme::instance(), &Theme::changed, body, [rebuild, tintFolder] {
        (*rebuild)();
        tintFolder();
    });
    return w;
}

QLabel *hint(const QString &text)
{
    auto *l = new QLabel(text);
    l->setObjectName(QStringLiteral("secondary"));
    l->setWordWrap(true);
    return l;
}

} // namespace

void SettingsDialog::showSingleton(QWidget *parent, const QString &page)
{
    static QPointer<SettingsDialog> dlg;
    if (!dlg)
        dlg = new SettingsDialog(parent);
    if (!page.isEmpty())
        if (auto *nav = dlg->findChild<QListWidget *>(QStringLiteral("settingsNav")))
            if (const auto found = nav->findItems(page, Qt::MatchExactly); !found.isEmpty())
                nav->setCurrentItem(found.first());
    dlg->show();
    dlg->raise();
    dlg->activateWindow();
}

SettingsDialog::SettingsDialog(QWidget *parent) : QDialog(parent, Qt::Window)
{
    setAttribute(Qt::WA_DeleteOnClose);
    setWindowTitle(Gifiles::tr("설정"));
    resize(760, 620);

    auto *nav = new QListWidget(this);
    nav->setObjectName(QStringLiteral("settingsNav"));
    nav->setFixedWidth(170);
    nav->setIconSize(QSize(18, 18));
    nav->setFrameShape(QFrame::NoFrame);
    auto *stack = new QStackedWidget(this);
    auto add = [&](const QString &glyph, const QString &name, QWidget *w) {
        auto *it = new QListWidgetItem(Theme::icon(glyph, Theme::colors().accent), name, nav);
        it->setSizeHint(QSize(150, 32));
        stack->addWidget(w);
    };

    QFormLayout *f;
    // 일반
    QWidget *general = page(Gifiles::tr("일반"), &f);
    f->addRow(Gifiles::tr("언어:"), choice(Settings::Language, {{Gifiles::tr("시스템 설정 따르기"), QStringLiteral("system")},
                                                    {QStringLiteral("한국어"), QStringLiteral("ko")},
                                                    {QStringLiteral("English"), QStringLiteral("en")},
                                                    {QStringLiteral("日本語"), QStringLiteral("ja")},
                                                    {QStringLiteral("简体中文"), QStringLiteral("zh_CN")}}, general));
    f->addRow(QString(), hint(Gifiles::tr("앱을 다시 시작하면 적용됩니다.")));
    f->addRow(Gifiles::tr("시작할 때:"), toggle(Settings::RestoreSession, Gifiles::tr("지난번 윈도우와 탭 복원"), general));
    f->addRow(Gifiles::tr("새로운 윈도우:"), choice(Settings::NewWindowHome, {{Gifiles::tr("현재 폴더 열기"), false},
                                                                       {Gifiles::tr("홈 폴더 열기"), true}}, general));
#ifdef Q_OS_MACOS
    auto *ask = new QCheckBox(Gifiles::tr("권한이 없으면 시작할 때 안내"), general);
    ask->setObjectName(QStringLiteral("switch"));
    follow(ask, Settings::DontAskFullDisk, [ask] { ask->setChecked(!Settings::instance()->flag(Settings::DontAskFullDisk)); });
    connect(ask, &QCheckBox::toggled, this, [](bool on) { Settings::instance()->setValue(Settings::DontAskFullDisk, !on); });
    f->addRow(Gifiles::tr("전체 디스크 접근:"), ask);
#endif
    // Where the settings live, for editing by hand or by an agent (see the file's own header).
    auto *pathRow = new QWidget(general);
    auto *pl = new QHBoxLayout(pathRow);
    pl->setContentsMargins(0, 0, 0, 0);
    auto *pathLabel = new QLabel(QDir::toNativeSeparators(Settings::configPath()), pathRow);
    pathLabel->setObjectName(QStringLiteral("secondary"));
    pathLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    auto *reveal = new QPushButton(util::revealShowText(), pathRow);
    connect(reveal, &QPushButton::clicked, this, [] {
        const util::Command c = util::revealCommand(Settings::configPath());
        if (!QProcess::startDetached(c.program, c.args))
            QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(Settings::configPath()).absolutePath()));
    });
    pl->addWidget(pathLabel);
    pl->addWidget(reveal);
    f->addRow(Gifiles::tr("설정 파일:"), pathRow);
    f->addRow(QString(), hint(Gifiles::tr("여기 있는 모든 설정과 단축키가 이 파일(config.toml)에 설명과 함께 저장됩니다. "
                                            "직접 고쳐 저장하면 바로 적용됩니다.")));
    add(QStringLiteral("home"), Gifiles::tr("일반"), general);

    // 보기
    QWidget *view = page(Gifiles::tr("보기"), &f);
    f->addRow(Gifiles::tr("기본 보기:"), choice(Settings::DefaultMode, {{Gifiles::tr("목록"), QStringLiteral("list")},
                                                                {Gifiles::tr("갤러리"), QStringLiteral("gallery")},
                                                                {Gifiles::tr("컬럼"), QStringLiteral("columns")}}, view));
    f->addRow(Gifiles::tr("정렬:"), toggle(Settings::FoldersFirst, Gifiles::tr("폴더를 항상 위에"), view));
    f->addRow(Gifiles::tr("목록:"), toggle(Settings::Stripes, Gifiles::tr("줄무늬 배경"), view));
    auto *hidden = new QCheckBox(Gifiles::tr("숨김 파일 보기"), view);
    hidden->setObjectName(QStringLiteral("switch"));
    hidden->setChecked(App::instance()->showHidden());
    connect(hidden, &QCheckBox::toggled, this, [](bool on) { App::instance()->setShowHidden(on); });
    connect(App::instance(), &App::showHiddenChanged, hidden, &QCheckBox::setChecked);
    f->addRow(QString(), hidden);
    auto *recent = new QSpinBox(view);
    recent->setObjectName(QStringLiteral("recentFolders"));
    recent->setRange(0, RecentFolders::kKept);
    recent->setSuffix(Gifiles::tr("개"));
    recent->setSpecialValueText(Gifiles::tr("숨김"));
    follow(recent, Settings::RecentFoldersCount,
           [recent] { recent->setValue(Settings::instance()->value(Settings::RecentFoldersCount).toInt()); });
    connect(recent, &QSpinBox::valueChanged, this, [](int v) { Settings::instance()->setValue(Settings::RecentFoldersCount, v); });
    f->addRow(Gifiles::tr("최근 폴더:"), recent);
    f->addRow(QString(), hint(Gifiles::tr("사이드바에 보일 개수. 둘러보기만 한 폴더가 아니라 파일을 복사·이동·만들기·삭제했거나, "
                                            "파일을 열었거나, 명령을 실행한 폴더만 들어갑니다.")));
    add(QStringLiteral("list"), Gifiles::tr("보기"), view);

    // 모양 및 색상
    add(QStringLiteral("palette"), Gifiles::tr("모양 및 색상"), appearancePage([](QWidget *page) {
        auto *look = new QWidget(page);
        auto *lf = new QFormLayout(look);
        lf->setContentsMargins(0, 0, 0, 0);
        lf->setHorizontalSpacing(18);
        lf->setVerticalSpacing(12);
        lf->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
        lf->setFieldGrowthPolicy(QFormLayout::FieldsStayAtSizeHint);
        lf->addRow(Gifiles::tr("테마:"), choice(Settings::ThemeMode, {{Gifiles::tr("시스템 설정 따르기"), QStringLiteral("system")},
                                                          {Gifiles::tr("라이트"), QStringLiteral("light")},
                                                          {Gifiles::tr("다크"), QStringLiteral("dark")}}, look));
        lf->addRow(Gifiles::tr("파일 이름:"), toggle(Settings::BoldNames, Gifiles::tr("굵게 보기"), look));
        lf->addRow(QString(), toggle(Settings::UppercaseNames, Gifiles::tr("항상 대문자로 표시"), look));
        return look;
    }));

    // 미리보기 (pane and Quick Look)
    QWidget *preview = page(Gifiles::tr("미리보기"), &f);
    auto sizeBox = [](const char *key, QWidget *parent) {
        auto *b = new QSpinBox(parent);
        b->setRange(9, 32);
        b->setSuffix(QStringLiteral(" pt"));
        follow(b, key, [b, key] { b->setValue(Settings::instance()->value(QString::fromLatin1(key)).toInt()); });
        QObject::connect(b, &QSpinBox::valueChanged, b, [key](int v) { Settings::instance()->setValue(QString::fromLatin1(key), v); });
        return b;
    };
    f->addRow(Gifiles::tr("텍스트 글꼴 크기:"), sizeBox(Settings::PreviewTextFontSize, preview));
    f->addRow(QString(), hint(Gifiles::tr("코드·설정 파일 등 (고정폭 글꼴)")));
    f->addRow(Gifiles::tr("문서 글꼴 크기:"), sizeBox(Settings::PreviewDocFontSize, preview));
    f->addRow(QString(), hint(Gifiles::tr("txt·md 등 글 위주 파일 (기본 글꼴, 줄 바꿈)")));
    add(QStringLiteral("eye"), Gifiles::tr("미리보기"), preview);

    // 터미널
    QWidget *term = page(Gifiles::tr("터미널"), &f);
    auto *shell = new QLineEdit(Settings::instance()->value(Settings::TermShell).toString(), term);
#ifdef Q_OS_WIN
    shell->setPlaceholderText(QStringLiteral("powershell.exe"));
#else
    shell->setPlaceholderText(qEnvironmentVariable("SHELL", QStringLiteral("/bin/zsh")));
#endif
    shell->setMinimumWidth(240);
    follow(shell, Settings::TermShell, [shell] { shell->setText(Settings::instance()->value(Settings::TermShell).toString()); });
    connect(shell, &QLineEdit::editingFinished, this, [shell] { Settings::instance()->setValue(Settings::TermShell, shell->text().trimmed()); });
    f->addRow(Gifiles::tr("셸:"), shell);
    auto *family = new QComboBox(term);
    family->addItem(Gifiles::tr("D2Coding (기본)"), QString());
    for (const QString &name : QFontDatabase::families())
        if (!QFontDatabase::isPrivateFamily(name) && name != QLatin1String("D2Coding")) // the default is the first item
            family->addItem(name, name);
    follow(family, Settings::TermFontFamily, [family] {
        const QString name = Settings::instance()->value(Settings::TermFontFamily).toString();
        int i = family->findData(name);
        if (i < 0) { // a font that isn't installed (any more): keep showing it
            family->addItem(name, name);
            i = family->count() - 1;
        }
        family->setCurrentIndex(i);
    });
    connect(family, &QComboBox::currentIndexChanged, this,
            [family] { Settings::instance()->setValue(Settings::TermFontFamily, family->currentData().toString()); });
    f->addRow(Gifiles::tr("글꼴:"), family);
    auto *size = new QSpinBox(term);
    size->setRange(9, 24);
    size->setSuffix(QStringLiteral(" pt"));
    follow(size, Settings::TermFontSize, [size] { size->setValue(Settings::instance()->value(Settings::TermFontSize).toInt()); });
    connect(size, &QSpinBox::valueChanged, this, [](int v) { Settings::instance()->setValue(Settings::TermFontSize, v); });
    f->addRow(Gifiles::tr("글꼴 크기:"), size);
    auto *lineHeight = new QSpinBox(term);
    lineHeight->setRange(100, 200);
    lineHeight->setSingleStep(5);
    lineHeight->setSuffix(QStringLiteral(" %"));
    follow(lineHeight, Settings::TermLineHeight,
           [lineHeight] { lineHeight->setValue(Settings::instance()->value(Settings::TermLineHeight).toInt()); });
    connect(lineHeight, &QSpinBox::valueChanged, this, [](int v) { Settings::instance()->setValue(Settings::TermLineHeight, v); });
    f->addRow(Gifiles::tr("줄간격:"), lineHeight);
    f->addRow(Gifiles::tr("연동:"), toggle(Settings::TermFollowFolder, Gifiles::tr("폴더를 옮기면 터미널도 이동 (cd)"), term));
    f->addRow(QString(), toggle(Settings::TermSyncBack, Gifiles::tr("터미널에서 cd 하면 목록도 이동"), term));
    f->addRow(QString(), hint(Gifiles::tr("예: rm -rf 를 입력한 뒤 목록에서 항목을 터미널로 끌어다 놓으면 경로가 붙고, Enter로 실행합니다. "
                                            "셸 변경은 터미널을 다시 열 때 적용됩니다.")));
    add(QStringLiteral("columns"), Gifiles::tr("터미널"), term);

    add(QStringLiteral("folder-plus"), Gifiles::tr("선택 항목 메뉴"), selectionMenuPage());
    add(QStringLiteral("keyboard"), Gifiles::tr("단축키"), shortcutsPage());

    auto *root = new QHBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);
    root->addWidget(nav);
    root->addWidget(stack, 1);
    connect(nav, &QListWidget::currentRowChanged, stack, &QStackedWidget::setCurrentIndex);
    nav->setCurrentRow(0);
}
