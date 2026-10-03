#pragma once

#include <QWidget>

class QHBoxLayout;
class QLineEdit;
class QFileSystemModel;

// Clickable breadcrumb. Clicking empty space (or ⇧⌘G) turns it into an editable path field
// with folder completion.
class PathBar : public QWidget {
    Q_OBJECT
public:
    explicit PathBar(QWidget *parent = nullptr);
    void setPath(const QString &path);
    void startEditing();

signals:
    void pathActivated(const QString &path);
    void editingFinished();

protected:
    void mousePressEvent(QMouseEvent *e) override;
    void resizeEvent(QResizeEvent *e) override;
    bool eventFilter(QObject *o, QEvent *e) override;

private:
    void rebuild();
    void fitSegments();
    void finishEditing(bool accept);

    QString m_path;
    QWidget *m_crumbs;
    QHBoxLayout *m_crumbLayout;
    QLineEdit *m_edit;
    QFileSystemModel *m_completionModel = nullptr;
};
