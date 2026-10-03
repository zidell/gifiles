#include "PathBar.h"
#include "Shortcuts.h"
#include "Util.h"

#include <QAbstractItemView>
#include <QCompleter>
#include <QDir>
#include <QFileIconProvider>
#include <QFileSystemModel>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QToolButton>

PathBar::PathBar(QWidget *parent) : QWidget(parent)
{
    auto *outer = new QHBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    m_crumbs = new QWidget(this);
    m_crumbLayout = new QHBoxLayout(m_crumbs);
    m_crumbLayout->setContentsMargins(4, 0, 4, 0);
    m_crumbLayout->setSpacing(0);
    m_edit = new QLineEdit(this);
    m_edit->setClearButtonEnabled(true);
    m_edit->hide();
    m_edit->installEventFilter(this);
    outer->addWidget(m_crumbs, 1);
    outer->addWidget(m_edit, 1);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setMinimumWidth(120);
}

void PathBar::setPath(const QString &path)
{
    if (path == m_path)
        return;
    m_path = path;
    rebuild();
}

void PathBar::rebuild()
{
    while (QLayoutItem *it = m_crumbLayout->takeAt(0)) {
        delete it->widget();
        delete it;
    }
    QStringList chain;
    QDir d(m_path);
    chain << d.absolutePath();
    while (d.cdUp())
        chain.prepend(d.absolutePath());
    QFileIconProvider icons;
    for (int i = 0; i < chain.size(); ++i) {
        if (i > 0) {
            auto *sep = new QLabel(QStringLiteral("›"), m_crumbs);
            sep->setObjectName(QStringLiteral("crumbSep"));
            m_crumbLayout->addWidget(sep);
        }
        auto *b = new QToolButton(m_crumbs);
        b->setAutoRaise(true);
        b->setObjectName(i == chain.size() - 1 ? QStringLiteral("crumbCurrent") : QStringLiteral("crumb"));
        b->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        b->setText(util::displayName(chain[i]));
        b->setIcon(icons.icon(QFileInfo(chain[i])));
        b->setIconSize(QSize(16, 16));
        b->setToolTip(QDir::toNativeSeparators(chain[i]));
        b->setFocusPolicy(Qt::NoFocus);
        const QString target = chain[i];
        connect(b, &QToolButton::clicked, this, [this, target] { emit pathActivated(target); });
        m_crumbLayout->addWidget(b);
    }
    m_crumbLayout->addStretch(1);
    fitSegments();
}

// Hide leading segments until the breadcrumb fits, Finder-style.
void PathBar::fitSegments()
{
    const int n = m_crumbLayout->count();
    for (int i = 0; i < n; ++i)
        if (QWidget *w = m_crumbLayout->itemAt(i)->widget())
            w->show();
    // Items alternate crumb, "›", crumb, ... (+ trailing stretch). Drop a crumb with its separator.
    int i = 0;
    while (m_crumbLayout->sizeHint().width() > width() && i + 2 < n - 2) {
        for (int k : {i, i + 1})
            if (QWidget *w = m_crumbLayout->itemAt(k)->widget())
                w->hide();
        i += 2;
    }
}

void PathBar::resizeEvent(QResizeEvent *e)
{
    QWidget::resizeEvent(e);
    fitSegments();
}

void PathBar::mousePressEvent(QMouseEvent *e)
{
    QWidget::mousePressEvent(e);
    startEditing();
}

void PathBar::startEditing()
{
    if (!m_completionModel) {
        m_completionModel = new QFileSystemModel(this);
        m_completionModel->setFilter(QDir::AllDirs | QDir::NoDotAndDotDot | QDir::Drives);
        m_completionModel->setRootPath(QString());
        auto *c = new QCompleter(m_completionModel, this);
        c->setCaseSensitivity(Qt::CaseInsensitive);
        m_edit->setCompleter(c);
    }
    QString p = QDir::toNativeSeparators(m_path);
    const QString home = QDir::homePath();
    if (util::isInside(m_path, home))
        p = QStringLiteral("~") + QDir::toNativeSeparators(m_path.mid(home.size()));
    if (!p.endsWith(QDir::separator()))
        p += QDir::separator();
    m_edit->setText(p);
    m_crumbs->hide();
    m_edit->show();
    m_edit->setFocus();
    m_edit->deselect();
    m_edit->setCursorPosition(p.size());
}

void PathBar::finishEditing(bool accept)
{
    if (!m_edit->isVisible())
        return;
    QString text = m_edit->text().trimmed();
    m_edit->hide();
    m_crumbs->show();
    fitSegments();
    if (accept && !text.isEmpty()) {
        if (text == QLatin1String("~") || text.startsWith(QLatin1String("~/")) || text.startsWith(QLatin1String("~\\")))
            text = QDir::homePath() + text.mid(1);
        QFileInfo fi(QDir(m_path).absoluteFilePath(QDir::fromNativeSeparators(text)));
        emit pathActivated(fi.absoluteFilePath());
    }
    emit editingFinished();
}

bool PathBar::eventFilter(QObject *o, QEvent *e)
{
    if (o == m_edit) {
        if (e->type() == QEvent::ShortcutOverride) {
            auto *key = static_cast<QKeyEvent *>(e);
            if (Shortcuts::instance()->matches(QStringLiteral("경로 입력 확인"), key)
                || Shortcuts::instance()->matches(QStringLiteral("경로 입력 취소"), key)) {
                e->accept();
                return true;
            }
        }
        if (e->type() == QEvent::KeyPress && Shortcuts::instance()->matches(QStringLiteral("경로 입력 취소"), static_cast<QKeyEvent *>(e))) {
            finishEditing(false);
            return true;
        }
        if (e->type() == QEvent::KeyPress && Shortcuts::instance()->matches(QStringLiteral("경로 입력 확인"), static_cast<QKeyEvent *>(e))) {
            finishEditing(true);
            return true;
        }
        if (e->type() == QEvent::FocusOut && !(m_edit->completer() && m_edit->completer()->popup()->isVisible()))
            finishEditing(false);
    }
    return QWidget::eventFilter(o, e);
}
