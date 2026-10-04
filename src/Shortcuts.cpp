#include "Shortcuts.h"
#include "Settings.h"
#include "Util.h"

#include <QAction>
#include <QKeyEvent>

namespace {

QString settingsKey(const QString &id) { return QStringLiteral("shortcuts/") + id; }

// The Korean input source turns ` into ₩; a key with ` also answers to ₩.
QList<QKeySequence> withAliases(const QList<QKeySequence> &keys)
{
    QList<QKeySequence> out;
    auto addOnce = [&out](const QKeySequence &k) {
        if (!k.isEmpty() && !out.contains(k))
            out << k;
    };
    for (const QKeySequence &k : keys) {
        addOnce(k);
        if (k.count() == 1 && k[0].key() == Qt::Key_Return)
            addOnce(QKeySequence(QKeyCombination(k[0].keyboardModifiers(), Qt::Key_Enter)));
        QString text = k.toString(QKeySequence::PortableText);
        if (text.contains(QLatin1Char('`')))
            addOnce(QKeySequence(text.replace(QLatin1Char('`'), QChar(0x20A9))));
    }
    return out;
}

} // namespace

const QList<Shortcuts::Entry> &Shortcuts::entries()
{
    // Qt writes ⌘ as "Ctrl" and the Control key as "Meta" on macOS. Keys that use the real
    // Control key on the Mac differ per platform; "{}" means no key.
#ifdef Q_OS_MACOS
    const bool mac = true;
#else
    const bool mac = false;
#endif
    auto e = [](const QString &id, const QString &group, QStringList keys, const QString &context = {}) {
        QList<QKeySequence> ks;
        for (const QString &k : keys)
            ks << QKeySequence(k, QKeySequence::PortableText);
        return Entry{id, group, ks, context};
    };
    using L = QStringList;
    static const QList<Entry> list = [&] {
        // Groups are the menus' titles (shown translated in Settings and config.toml).
        const QString file = QString::fromUtf8(QT_TRANSLATE_NOOP("Gifiles", "파일")),
                      edit = QString::fromUtf8(QT_TRANSLATE_NOOP("Gifiles", "편집")),
                      view = QString::fromUtf8(QT_TRANSLATE_NOOP("Gifiles", "보기")),
                      go = QString::fromUtf8(QT_TRANSLATE_NOOP("Gifiles", "이동")),
                      win = QString::fromUtf8(QT_TRANSLATE_NOOP("Gifiles", "윈도우"));
        QList<Entry> l = {
            e(QString::fromUtf8(QT_TRANSLATE_NOOP("Gifiles", "새로운 윈도우")), file, L{"Ctrl+N"}),
            e(QString::fromUtf8(QT_TRANSLATE_NOOP("Gifiles", "새로운 탭")), file, L{"Ctrl+T"}),
            e(QString::fromUtf8(QT_TRANSLATE_NOOP("Gifiles", "새로운 폴더")), file, L{"Ctrl+Shift+N"}),
            e(QString::fromUtf8(QT_TRANSLATE_NOOP("Gifiles", "선택 항목으로 새로운 폴더")), file, mac ? L{"Ctrl+Meta+N"} : L{"Ctrl+Alt+N"}),
            e(QString::fromUtf8(QT_TRANSLATE_NOOP("Gifiles", "열기")), file, L{"Ctrl+Down"}, QStringLiteral("files")),
            e(QString::fromUtf8(QT_TRANSLATE_NOOP("Gifiles", "다음으로 열기…")), file, L{}),
            e(QString::fromUtf8(QT_TRANSLATE_NOOP("Gifiles", "새로운 탭에서 열기")), file, mac ? L{"Ctrl+Meta+O"} : L{"Ctrl+Alt+O"}),
            e(QString::fromUtf8(QT_TRANSLATE_NOOP("Gifiles", "탭 닫기")), file, L{"Ctrl+W"}),
            e(QString::fromUtf8(QT_TRANSLATE_NOOP("Gifiles", "윈도우 닫기")), file, L{"Ctrl+Shift+W"}),
            e(QString::fromUtf8(QT_TRANSLATE_NOOP("Gifiles", "정보 가져오기")), file, L{"Ctrl+I"}),
            e(util::revealActionId(), file, L{}),
            e(QString::fromUtf8(QT_TRANSLATE_NOOP("Gifiles", "이름 변경")), file, L{"Return"}, QStringLiteral("files")),
            e(QString::fromUtf8(QT_TRANSLATE_NOOP("Gifiles", "복제")), file, L{"Ctrl+D"}),
            // Quick Look keys are configurable, including the unmodified Space key.
            e(QString::fromUtf8(QT_TRANSLATE_NOOP("Gifiles", "퀵 뷰어")), file, L{"Space", "Ctrl+Y"}, QStringLiteral("files")),
            e(QString::fromUtf8(QT_TRANSLATE_NOOP("Gifiles", "휴지통으로 이동")), file, L{"Ctrl+Backspace"}),
            e(QString::fromUtf8(QT_TRANSLATE_NOOP("Gifiles", "설정…")), file, L{"Ctrl+,"}),
            e(QString::fromUtf8(QT_TRANSLATE_NOOP("Gifiles", "업데이트 확인…")), file, L{}),
            e(QString::fromUtf8(QT_TRANSLATE_NOOP("Gifiles", "종료")), file, L{"Ctrl+Q"}),
            e(QString::fromUtf8(QT_TRANSLATE_NOOP("Gifiles", "실행 취소")), edit, L{"Ctrl+Z"}),
            e(QString::fromUtf8(QT_TRANSLATE_NOOP("Gifiles", "실행 복귀")), edit, L{"Ctrl+Shift+Z"}),
            e(QString::fromUtf8(QT_TRANSLATE_NOOP("Gifiles", "잘라내기")), edit, L{"Ctrl+X"}),
            e(QString::fromUtf8(QT_TRANSLATE_NOOP("Gifiles", "복사하기")), edit, L{"Ctrl+C"}),
            e(QString::fromUtf8(QT_TRANSLATE_NOOP("Gifiles", "붙여넣기")), edit, L{"Ctrl+V"}),
            e(QString::fromUtf8(QT_TRANSLATE_NOOP("Gifiles", "항목을 여기로 이동")), edit, L{"Ctrl+Alt+V"}),
            e(QString::fromUtf8(QT_TRANSLATE_NOOP("Gifiles", "경로 복사")), edit, L{"Ctrl+Alt+C"}),
            e(QString::fromUtf8(QT_TRANSLATE_NOOP("Gifiles", "전체 선택")), edit, L{"Ctrl+A"}),
            e(QString::fromUtf8(QT_TRANSLATE_NOOP("Gifiles", "찾기")), edit, L{"Ctrl+F"}),
            // Same numbers as Finder: ⌘1 icons (gallery here), ⌘2 list, ⌘3 columns.
            e(QString::fromUtf8(QT_TRANSLATE_NOOP("Gifiles", "갤러리로")), view, L{"Ctrl+1"}),
            e(QString::fromUtf8(QT_TRANSLATE_NOOP("Gifiles", "목록으로")), view, L{"Ctrl+2"}),
            e(QString::fromUtf8(QT_TRANSLATE_NOOP("Gifiles", "컬럼으로")), view, L{"Ctrl+3"}),
            e(QString::fromUtf8(QT_TRANSLATE_NOOP("Gifiles", "숨김 파일 보기")), view, L{"Ctrl+Shift+.", "Ctrl+Shift+>", "Ctrl+>"}),
            e(QString::fromUtf8(QT_TRANSLATE_NOOP("Gifiles", "사이드바 보기")), view, mac ? L{"Ctrl+Meta+S"} : L{"Ctrl+Alt+S"}),
            e(QString::fromUtf8(QT_TRANSLATE_NOOP("Gifiles", "미리보기 보기")), view, L{"Ctrl+Shift+P"}),
            e(QString::fromUtf8(QT_TRANSLATE_NOOP("Gifiles", "터미널 펼치기")), view, mac ? L{"Meta+`"} : L{"Ctrl+`"}),
            e(QString::fromUtf8(QT_TRANSLATE_NOOP("Gifiles", "사이드바로 이동")), view, L{"Alt+Left"}),
            e(QString::fromUtf8(QT_TRANSLATE_NOOP("Gifiles", "파일뷰로 이동")), view, L{"Alt+Right", "Alt+Up"}),
            e(QString::fromUtf8(QT_TRANSLATE_NOOP("Gifiles", "터미널로 이동")), view, L{"Alt+Down"}),
            e(QString::fromUtf8(QT_TRANSLATE_NOOP("Gifiles", "글꼴 크게")), view, L{"Ctrl++", "Ctrl+="}),
            e(QString::fromUtf8(QT_TRANSLATE_NOOP("Gifiles", "글꼴 작게")), view, L{"Ctrl+-"}),
            e(QString::fromUtf8(QT_TRANSLATE_NOOP("Gifiles", "기본 글꼴 크기")), view, L{"Ctrl+0"}),
            e(QString::fromUtf8(QT_TRANSLATE_NOOP("Gifiles", "아이콘 크게")), view, L{"Ctrl+Alt+=", "Ctrl+Alt++"}),
            e(QString::fromUtf8(QT_TRANSLATE_NOOP("Gifiles", "아이콘 작게")), view, L{"Ctrl+Alt+-"}),
            e(QString::fromUtf8(QT_TRANSLATE_NOOP("Gifiles", "뒤로")), go, L{"Ctrl+[", "Ctrl+Left"}),
            e(QString::fromUtf8(QT_TRANSLATE_NOOP("Gifiles", "앞으로")), go, L{"Ctrl+]", "Ctrl+Right"}),
            e(QString::fromUtf8(QT_TRANSLATE_NOOP("Gifiles", "상위 폴더")), go, L{"Ctrl+Up"}),
            e(QString::fromUtf8(QT_TRANSLATE_NOOP("Gifiles", "홈")), go, L{"Ctrl+Shift+H"}),
            e(QString::fromUtf8(QT_TRANSLATE_NOOP("Gifiles", "데스크탑")), go, L{"Ctrl+Shift+D"}),
            e(QString::fromUtf8(QT_TRANSLATE_NOOP("Gifiles", "문서")), go, L{"Ctrl+Shift+O"}),
            e(QString::fromUtf8(QT_TRANSLATE_NOOP("Gifiles", "다운로드")), go, L{"Ctrl+Alt+L"}),
        };
        if (mac)
            l << e(QString::fromUtf8(QT_TRANSLATE_NOOP("Gifiles", "응용 프로그램")), go, L{"Ctrl+Shift+A"});
        l << e(QString::fromUtf8(QT_TRANSLATE_NOOP("Gifiles", "폴더로 이동…")), go, L{"Ctrl+Shift+G"})
          << e(QString::fromUtf8(QT_TRANSLATE_NOOP("Gifiles", "폴더 트리")), go, L{"`"}, QStringLiteral("files"))
          << e(QString::fromUtf8(QT_TRANSLATE_NOOP("Gifiles", "최소화")), win, L{"Ctrl+M"})
          << e(QString::fromUtf8(QT_TRANSLATE_NOOP("Gifiles", "다음 탭 보기")), win, mac ? L{"Meta+Tab", "Ctrl+}"} : L{"Ctrl+Tab", "Ctrl+}"})
          << e(QString::fromUtf8(QT_TRANSLATE_NOOP("Gifiles", "이전 탭 보기")), win, mac ? L{"Meta+Shift+Tab", "Ctrl+{"} : L{"Ctrl+Shift+Tab", "Ctrl+{"})
          << e(QString::fromUtf8(QT_TRANSLATE_NOOP("Gifiles", "전체 디스크 접근 권한…")), win, L{});
        l << e(QString::fromUtf8(QT_TRANSLATE_NOOP("Gifiles", "폴더 펼치기")), QString::fromUtf8(QT_TRANSLATE_NOOP("Gifiles", "파일뷰 조작")), L{"Right"}, QStringLiteral("files"));
        l << e(QString::fromUtf8(QT_TRANSLATE_NOOP("Gifiles", "폴더 접기")), QString::fromUtf8(QT_TRANSLATE_NOOP("Gifiles", "파일뷰 조작")), L{"Left"}, QStringLiteral("files"));
        l << e(QString::fromUtf8(QT_TRANSLATE_NOOP("Gifiles", "하위 폴더 모두 펼치기")), QString::fromUtf8(QT_TRANSLATE_NOOP("Gifiles", "파일뷰 조작")), L{"Ctrl+Alt+Right"}, QStringLiteral("files"));
        l << e(QString::fromUtf8(QT_TRANSLATE_NOOP("Gifiles", "하위 폴더 모두 접기")), QString::fromUtf8(QT_TRANSLATE_NOOP("Gifiles", "파일뷰 조작")), L{"Ctrl+Alt+Left"}, QStringLiteral("files"));
        l << e(QString::fromUtf8(QT_TRANSLATE_NOOP("Gifiles", "사이드바 항목 열기")), QString::fromUtf8(QT_TRANSLATE_NOOP("Gifiles", "사이드바 조작")), L{"Return"}, QStringLiteral("sidebar"));
        l << e(QString::fromUtf8(QT_TRANSLATE_NOOP("Gifiles", "사이드바에서 파일뷰로 이동")), QString::fromUtf8(QT_TRANSLATE_NOOP("Gifiles", "사이드바 조작")), L{"Right"}, QStringLiteral("sidebar"));
        l << e(QString::fromUtf8(QT_TRANSLATE_NOOP("Gifiles", "검색에서 파일뷰로 이동")), QString::fromUtf8(QT_TRANSLATE_NOOP("Gifiles", "검색 조작")), L{"Down", "Return"}, QStringLiteral("search"));
        l << e(QString::fromUtf8(QT_TRANSLATE_NOOP("Gifiles", "검색 취소")), QString::fromUtf8(QT_TRANSLATE_NOOP("Gifiles", "검색 조작")), L{"Esc"}, QStringLiteral("search"));
        l << e(QString::fromUtf8(QT_TRANSLATE_NOOP("Gifiles", "경로 입력 확인")), QString::fromUtf8(QT_TRANSLATE_NOOP("Gifiles", "경로 입력 조작")), L{"Return"}, QStringLiteral("path"));
        l << e(QString::fromUtf8(QT_TRANSLATE_NOOP("Gifiles", "경로 입력 취소")), QString::fromUtf8(QT_TRANSLATE_NOOP("Gifiles", "경로 입력 조작")), L{"Esc"}, QStringLiteral("path"));
        l << e(QString::fromUtf8(QT_TRANSLATE_NOOP("Gifiles", "퀵 뷰어 닫기")), QString::fromUtf8(QT_TRANSLATE_NOOP("Gifiles", "퀵 뷰어 조작")), L{"Space", "Esc", "Ctrl+W"}, QStringLiteral("preview"));
        l << e(QString::fromUtf8(QT_TRANSLATE_NOOP("Gifiles", "퀵 뷰어 확대")), QString::fromUtf8(QT_TRANSLATE_NOOP("Gifiles", "퀵 뷰어 조작")), L{"+", "=", "Ctrl++", "Ctrl+="}, QStringLiteral("preview"));
        l << e(QString::fromUtf8(QT_TRANSLATE_NOOP("Gifiles", "퀵 뷰어 축소")), QString::fromUtf8(QT_TRANSLATE_NOOP("Gifiles", "퀵 뷰어 조작")), L{"-", "Ctrl+-"}, QStringLiteral("preview"));
        l << e(QString::fromUtf8(QT_TRANSLATE_NOOP("Gifiles", "퀵 뷰어 이전 항목")), QString::fromUtf8(QT_TRANSLATE_NOOP("Gifiles", "퀵 뷰어 조작")), L{"Up", "Left"}, QStringLiteral("preview"));
        l << e(QString::fromUtf8(QT_TRANSLATE_NOOP("Gifiles", "퀵 뷰어 다음 항목")), QString::fromUtf8(QT_TRANSLATE_NOOP("Gifiles", "퀵 뷰어 조작")), L{"Down", "Right"}, QStringLiteral("preview"));
        l << e(QString::fromUtf8(QT_TRANSLATE_NOOP("Gifiles", "미디어 뒤로 탐색")), QString::fromUtf8(QT_TRANSLATE_NOOP("Gifiles", "퀵 뷰어 조작")), L{"Left"}, QStringLiteral("preview-media"));
        l << e(QString::fromUtf8(QT_TRANSLATE_NOOP("Gifiles", "미디어 앞으로 탐색")), QString::fromUtf8(QT_TRANSLATE_NOOP("Gifiles", "퀵 뷰어 조작")), L{"Right"}, QStringLiteral("preview-media"));
        l << e(QString::fromUtf8(QT_TRANSLATE_NOOP("Gifiles", "미디어 재생·일시정지")), QString::fromUtf8(QT_TRANSLATE_NOOP("Gifiles", "퀵 뷰어 조작")), L{"Return"}, QStringLiteral("preview"));
        l << e(QString::fromUtf8(QT_TRANSLATE_NOOP("Gifiles", "터미널 새 탭")), QString::fromUtf8(QT_TRANSLATE_NOOP("Gifiles", "터미널 조작")), L{"Ctrl+T"}, QStringLiteral("terminal"));
        l << e(QString::fromUtf8(QT_TRANSLATE_NOOP("Gifiles", "터미널 탭 닫기")), QString::fromUtf8(QT_TRANSLATE_NOOP("Gifiles", "터미널 조작")), L{"Ctrl+W"}, QStringLiteral("terminal"));
        l << e(QString::fromUtf8(QT_TRANSLATE_NOOP("Gifiles", "터미널 복사")), QString::fromUtf8(QT_TRANSLATE_NOOP("Gifiles", "터미널 조작")), L{"Ctrl+C"}, QStringLiteral("terminal"));
        l << e(QString::fromUtf8(QT_TRANSLATE_NOOP("Gifiles", "터미널 붙여넣기")), QString::fromUtf8(QT_TRANSLATE_NOOP("Gifiles", "터미널 조작")), L{"Ctrl+V"}, QStringLiteral("terminal"));
        l << e(QString::fromUtf8(QT_TRANSLATE_NOOP("Gifiles", "터미널 화면 지우기")), QString::fromUtf8(QT_TRANSLATE_NOOP("Gifiles", "터미널 조작")), L{"Ctrl+K"}, QStringLiteral("terminal"));
        l << e(QString::fromUtf8(QT_TRANSLATE_NOOP("Gifiles", "터미널 입력줄 지우기")), QString::fromUtf8(QT_TRANSLATE_NOOP("Gifiles", "터미널 조작")), L{"Ctrl+Backspace"}, QStringLiteral("terminal"));
        l << e(QString::fromUtf8(QT_TRANSLATE_NOOP("Gifiles", "터미널 줄 처음")), QString::fromUtf8(QT_TRANSLATE_NOOP("Gifiles", "터미널 조작")), L{"Ctrl+Left"}, QStringLiteral("terminal"));
        l << e(QString::fromUtf8(QT_TRANSLATE_NOOP("Gifiles", "터미널 줄 끝")), QString::fromUtf8(QT_TRANSLATE_NOOP("Gifiles", "터미널 조작")), L{"Ctrl+Right"}, QStringLiteral("terminal"));
        l << e(QString::fromUtf8(QT_TRANSLATE_NOOP("Gifiles", "터미널 이전 단어")), QString::fromUtf8(QT_TRANSLATE_NOOP("Gifiles", "터미널 조작")), L{}, QStringLiteral("terminal"));
        l << e(QString::fromUtf8(QT_TRANSLATE_NOOP("Gifiles", "터미널 다음 단어")), QString::fromUtf8(QT_TRANSLATE_NOOP("Gifiles", "터미널 조작")), L{}, QStringLiteral("terminal"));
        l << e(QString::fromUtf8(QT_TRANSLATE_NOOP("Gifiles", "터미널 이전 페이지")), QString::fromUtf8(QT_TRANSLATE_NOOP("Gifiles", "터미널 조작")), L{"Shift+PgUp"}, QStringLiteral("terminal"));
        l << e(QString::fromUtf8(QT_TRANSLATE_NOOP("Gifiles", "터미널 다음 페이지")), QString::fromUtf8(QT_TRANSLATE_NOOP("Gifiles", "터미널 조작")), L{"Shift+PgDown"}, QStringLiteral("terminal"));
        l << e(QString::fromUtf8(QT_TRANSLATE_NOOP("Gifiles", "터미널 다시 시작")), QString::fromUtf8(QT_TRANSLATE_NOOP("Gifiles", "터미널 조작")), L{"Return"}, QStringLiteral("terminal"));
        l << e(QString::fromUtf8(QT_TRANSLATE_NOOP("Gifiles", "이름 변경 확정")), QString::fromUtf8(QT_TRANSLATE_NOOP("Gifiles", "이름 변경 조작")), L{"Return"}, QStringLiteral("rename"));
        l << e(QString::fromUtf8(QT_TRANSLATE_NOOP("Gifiles", "이름 변경 취소")), QString::fromUtf8(QT_TRANSLATE_NOOP("Gifiles", "이름 변경 조작")), L{"Esc"}, QStringLiteral("rename"));
        l << e(QString::fromUtf8(QT_TRANSLATE_NOOP("Gifiles", "컨텍스트 메뉴 열기")), QString::fromUtf8(QT_TRANSLATE_NOOP("Gifiles", "컨텍스트 메뉴 조작")), L{"O"}, QStringLiteral("menu"));
        l << e(QString::fromUtf8(QT_TRANSLATE_NOOP("Gifiles", "컨텍스트 메뉴 다음으로 열기")), QString::fromUtf8(QT_TRANSLATE_NOOP("Gifiles", "컨텍스트 메뉴 조작")), L{"H"}, QStringLiteral("menu"));
        l << e(QString::fromUtf8(QT_TRANSLATE_NOOP("Gifiles", "컨텍스트 메뉴 새로운 탭에서 열기")), QString::fromUtf8(QT_TRANSLATE_NOOP("Gifiles", "컨텍스트 메뉴 조작")), L{"E"}, QStringLiteral("menu"));
        l << e(QString::fromUtf8(QT_TRANSLATE_NOOP("Gifiles", "컨텍스트 메뉴 퀵 뷰어")), QString::fromUtf8(QT_TRANSLATE_NOOP("Gifiles", "컨텍스트 메뉴 조작")), L{"Q"}, QStringLiteral("menu"));
        l << e(QString::fromUtf8(QT_TRANSLATE_NOOP("Gifiles", "컨텍스트 메뉴 외부 파일관리자")), QString::fromUtf8(QT_TRANSLATE_NOOP("Gifiles", "컨텍스트 메뉴 조작")), L{"I"}, QStringLiteral("menu"));
        l << e(QString::fromUtf8(QT_TRANSLATE_NOOP("Gifiles", "컨텍스트 메뉴 이름 변경")), QString::fromUtf8(QT_TRANSLATE_NOOP("Gifiles", "컨텍스트 메뉴 조작")), L{"M"}, QStringLiteral("menu"));
        l << e(QString::fromUtf8(QT_TRANSLATE_NOOP("Gifiles", "컨텍스트 메뉴 복제")), QString::fromUtf8(QT_TRANSLATE_NOOP("Gifiles", "컨텍스트 메뉴 조작")), L{"U"}, QStringLiteral("menu"));
        l << e(QString::fromUtf8(QT_TRANSLATE_NOOP("Gifiles", "컨텍스트 메뉴 선택 항목 명령")), QString::fromUtf8(QT_TRANSLATE_NOOP("Gifiles", "컨텍스트 메뉴 조작")), L{"S"}, QStringLiteral("menu"));
        l << e(QString::fromUtf8(QT_TRANSLATE_NOOP("Gifiles", "컨텍스트 메뉴 명령 편집")), QString::fromUtf8(QT_TRANSLATE_NOOP("Gifiles", "컨텍스트 메뉴 조작")), L{"E"}, QStringLiteral("menu"));
        l << e(QString::fromUtf8(QT_TRANSLATE_NOOP("Gifiles", "컨텍스트 메뉴 경로 복사")), QString::fromUtf8(QT_TRANSLATE_NOOP("Gifiles", "컨텍스트 메뉴 조작")), L{"A"}, QStringLiteral("menu"));
        l << e(QString::fromUtf8(QT_TRANSLATE_NOOP("Gifiles", "컨텍스트 메뉴 잘라내기")), QString::fromUtf8(QT_TRANSLATE_NOOP("Gifiles", "컨텍스트 메뉴 조작")), L{"T"}, QStringLiteral("menu"));
        l << e(QString::fromUtf8(QT_TRANSLATE_NOOP("Gifiles", "컨텍스트 메뉴 복사")), QString::fromUtf8(QT_TRANSLATE_NOOP("Gifiles", "컨텍스트 메뉴 조작")), L{"C"}, QStringLiteral("menu"));
        l << e(QString::fromUtf8(QT_TRANSLATE_NOOP("Gifiles", "컨텍스트 메뉴 휴지통")), QString::fromUtf8(QT_TRANSLATE_NOOP("Gifiles", "컨텍스트 메뉴 조작")), L{"D"}, QStringLiteral("menu"));
        l << e(QString::fromUtf8(QT_TRANSLATE_NOOP("Gifiles", "컨텍스트 메뉴 새 폴더")), QString::fromUtf8(QT_TRANSLATE_NOOP("Gifiles", "컨텍스트 메뉴 조작")), L{"F"}, QStringLiteral("menu"));
        l << e(QString::fromUtf8(QT_TRANSLATE_NOOP("Gifiles", "컨텍스트 메뉴 붙여넣기")), QString::fromUtf8(QT_TRANSLATE_NOOP("Gifiles", "컨텍스트 메뉴 조작")), L{"P"}, QStringLiteral("menu"));
        l << e(QString::fromUtf8(QT_TRANSLATE_NOOP("Gifiles", "컨텍스트 메뉴 이동")), QString::fromUtf8(QT_TRANSLATE_NOOP("Gifiles", "컨텍스트 메뉴 조작")), L{"M"}, QStringLiteral("menu"));
        l << e(QString::fromUtf8(QT_TRANSLATE_NOOP("Gifiles", "컨텍스트 메뉴 갤러리")), QString::fromUtf8(QT_TRANSLATE_NOOP("Gifiles", "컨텍스트 메뉴 조작")), L{"1"}, QStringLiteral("menu"));
        l << e(QString::fromUtf8(QT_TRANSLATE_NOOP("Gifiles", "컨텍스트 메뉴 목록")), QString::fromUtf8(QT_TRANSLATE_NOOP("Gifiles", "컨텍스트 메뉴 조작")), L{"2"}, QStringLiteral("menu"));
        l << e(QString::fromUtf8(QT_TRANSLATE_NOOP("Gifiles", "컨텍스트 메뉴 컬럼")), QString::fromUtf8(QT_TRANSLATE_NOOP("Gifiles", "컨텍스트 메뉴 조작")), L{"3"}, QStringLiteral("menu"));
        l << e(QString::fromUtf8(QT_TRANSLATE_NOOP("Gifiles", "컨텍스트 메뉴 숨김 파일")), QString::fromUtf8(QT_TRANSLATE_NOOP("Gifiles", "컨텍스트 메뉴 조작")), L{"H"}, QStringLiteral("menu"));
        return l;
    }();
    return list;
}

Shortcuts *Shortcuts::instance()
{
    static Shortcuts *s = new Shortcuts;
    return s;
}

Shortcuts::Shortcuts()
{
    // Edits of config.toml (in the app or outside it) reach every window's actions.
    connect(Settings::instance(), &Settings::changed, this, [this](const QString &key) {
        if (key.startsWith(QLatin1String("shortcuts/")) || key.isEmpty()) {
            applyAll();
            emit changed();
        }
    });
}

void Shortcuts::add(QAction *action, const QString &id)
{
    Q_ASSERT_X(std::any_of(entries().begin(), entries().end(), [&](const Entry &e) { return e.id == id; }),
               "Shortcuts::add", qPrintable(id));
    m_actions.insert(id, action);
    action->setShortcuts(withAliases(keys(id)));
}

QString Shortcuts::currentId(const QString &id)
{
    // 훑어보기 became 퀵 뷰어; the context menu's 새 탭에서 열기 took the menu's name.
    static const QString oldQuick = QStringLiteral("훑어보기");
    if (id.contains(oldQuick))
        return QString(id).replace(oldQuick, QStringLiteral("퀵 뷰어"));
    if (id == QStringLiteral("컨텍스트 메뉴 새 탭에서 열기"))
        return QStringLiteral("컨텍스트 메뉴 새로운 탭에서 열기");
    return id;
}

QList<QKeySequence> Shortcuts::defaults(const QString &id) const
{
    for (const Entry &e : entries())
        if (e.id == id)
            return e.defaults;
    return {};
}

QString Shortcuts::context(const QString &id) const
{
    for (const Entry &e : entries())
        if (e.id == id)
            return e.context;
    return {};
}

bool Shortcuts::conflicts(const QString &first, const QString &second) const
{
    const QString a = context(first), b = context(second);
    return a == b || (a.isEmpty() && (b == QLatin1String("files") || b == QLatin1String("search")))
                  || (b.isEmpty() && (a == QLatin1String("files") || a == QLatin1String("search")));
}

QList<QKeySequence> Shortcuts::keys(const QString &id) const
{
    if (!isCustom(id))
        return defaults(id);
    QList<QKeySequence> out;
    for (const QString &s : Settings::instance()->value(settingsKey(id)).toStringList())
        if (const QKeySequence k(s, QKeySequence::PortableText); !k.isEmpty())
            out << k;
    return out;
}

bool Shortcuts::isCustom(const QString &id) const { return Settings::instance()->contains(settingsKey(id)); }

bool Shortcuts::matches(const QString &id, const QKeyEvent *event) const
{
    const QKeySequence pressed(QKeyCombination(event->modifiers() & ~Qt::KeypadModifier, Qt::Key(event->key())));
    for (const QKeySequence &k : withAliases(keys(id)))
        if (k.matches(pressed) == QKeySequence::ExactMatch)
            return true;
    return false;
}

QStringList Shortcuts::toStrings(const QList<QKeySequence> &keys)
{
    QStringList list;
    for (const QKeySequence &k : keys)
        list << k.toString(QKeySequence::PortableText);
    return list;
}

QStringList Shortcuts::assign(const QString &id, const QKeySequence &key)
{
    QStringList takenFrom;
    for (const Entry &e : entries()) {
        if (e.id == id || !conflicts(e.id, id))
            continue;
        QList<QKeySequence> ks = keys(e.id);
        if (ks.removeAll(key) > 0) {
            store(e.id, ks);
            takenFrom << e.id;
        }
    }
    store(id, {key});
    return takenFrom;
}

void Shortcuts::reset(const QString &id)
{
    // The built-in keys may be in use by another action by now; take them back.
    const QList<QKeySequence> ds = defaults(id);
    for (const Entry &e : entries()) {
        if (e.id == id || !conflicts(e.id, id) || !isCustom(e.id))
            continue;
        QList<QKeySequence> ks = keys(e.id);
        bool removed = false;
        for (const QKeySequence &k : ds)
            removed = ks.removeAll(k) > 0 || removed;
        if (removed)
            store(e.id, ks);
    }
    Settings::instance()->remove(settingsKey(id));
}

void Shortcuts::resetAll()
{
    for (const Entry &e : entries())
        Settings::instance()->remove(settingsKey(e.id));
}

void Shortcuts::clearAll()
{
    for (const Entry &e : entries())
        store(e.id, {});
}

void Shortcuts::store(const QString &id, const QList<QKeySequence> &ks)
{
    if (ks == defaults(id))
        Settings::instance()->remove(settingsKey(id));
    else
        Settings::instance()->setValue(settingsKey(id), toStrings(ks));
}

void Shortcuts::applyAll()
{
    for (auto it = m_actions.cbegin(); it != m_actions.cend(); ++it)
        if (it.value())
            it.value()->setShortcuts(withAliases(keys(it.key())));
}
