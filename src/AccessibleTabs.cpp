#include "AccessibleTabs.h"

#include <QAccessibleWidget>
#include <QHash>
#include <QPointer>
#include <QTabBar>

namespace {
class AccessibleTab final : public QAccessibleInterface, public QAccessibleActionInterface {
    QPointer<QTabBar> bar;
    int index;
public:
    AccessibleTab(QTabBar *b, int i) : bar(b), index(i) {}
    bool isValid() const override { return bar && index >= 0 && index < bar->count(); }
    QObject *object() const override { return nullptr; }
    QAccessible::Role role() const override { return QAccessible::PageTab; }
    QAccessibleInterface *parent() const override { return QAccessible::queryAccessibleInterface(bar); }
    QRect rect() const override {
        if (!isValid()) return {};
        return bar->tabRect(index).translated(bar->mapToGlobal(QPoint()));
    }
    QAccessible::State state() const override {
        QAccessible::State s;
        if (!isValid()) { s.invalid = true; return s; }
        s = parent()->state();
        s.selectable = true;
        s.selected = index == bar->currentIndex();
        s.focused = s.selected; // Match Qt's synthetic-tab semantics, including NoFocus bars.
        s.disabled |= !bar->isTabEnabled(index);
        s.invisible |= !bar->isTabVisible(index);
        return s;
    }
    QString text(QAccessible::Text type) const override {
        if (!isValid()) return {};
        if (type == QAccessible::Name) {
            auto name = bar->accessibleTabName(index);
            return name.isEmpty() ? bar->tabText(index) : name;
        }
        if (type == QAccessible::Description) return bar->tabToolTip(index);
        if (type == QAccessible::Help) return bar->tabWhatsThis(index);
        return {};
    }
    void setText(QAccessible::Text, const QString &) override {}
    QAccessibleInterface *childAt(int, int) const override { return nullptr; }
    QAccessibleInterface *child(int) const override { return nullptr; }
    int childCount() const override { return 0; }
    int indexOfChild(const QAccessibleInterface *) const override { return -1; }
    void *interface_cast(QAccessible::InterfaceType type) override {
        return type == QAccessible::ActionInterface ? static_cast<QAccessibleActionInterface *>(this) : nullptr;
    }
    QStringList actionNames() const override { return {pressAction()}; }
    void doAction(const QString &action) override {
        if (isValid() && bar->isEnabled() && bar->isTabEnabled(index)
            && bar->isTabVisible(index) && action == pressAction()) bar->setCurrentIndex(index);
    }
    QStringList keyBindingsForAction(const QString &) const override { return {}; }
};

class AccessibleTabBar final : public QAccessibleWidget, public QAccessibleSelectionInterface {
    mutable QHash<int, QAccessible::Id> tabs;
    QTabBar *bar() const { return static_cast<QTabBar *>(object()); }
    QList<QWidget *> controls() const {
        // Real widgets keep their real parent, geometry, visibility and actions.
        // Include scroll buttons and either-side/custom tab buttons exactly once.
        return bar()->findChildren<QWidget *>(QString(), Qt::FindDirectChildrenOnly);
    }
public:
    explicit AccessibleTabBar(QTabBar *b) : QAccessibleWidget(b, QAccessible::PageTabList) {}
    ~AccessibleTabBar() override {
        for (auto id : std::as_const(tabs)) QAccessible::deleteAccessibleInterface(id);
    }
    QAccessibleInterface *child(int index) const override {
        if (index < 0) return nullptr;
        if (index < bar()->count()) {
            auto id = tabs.value(index);
            if (auto *tab = QAccessible::accessibleInterface(id)) return tab;
            auto *tab = new AccessibleTab(bar(), index);
            tabs.insert(index, QAccessible::registerAccessibleInterface(tab));
            return tab;
        }
        auto widgets = controls(); index -= bar()->count();
        return index < widgets.size() ? QAccessible::queryAccessibleInterface(widgets[index]) : nullptr;
    }
    int childCount() const override { return bar()->count() + controls().size(); }
    int indexOfChild(const QAccessibleInterface *iface) const override {
        if (!iface) return -1;
        if (iface->object()) {
            int i = controls().indexOf(qobject_cast<QWidget *>(iface->object()));
            return i < 0 ? -1 : bar()->count() + i;
        }
        for (int i = 0; i < bar()->count(); ++i) if (child(i) == iface) return i;
        return -1;
    }
    QAccessibleInterface *childAt(int x, int y) const override {
        // A button inside a tab must win over the containing tab's rectangle.
        for (int i = bar()->count(); i < childCount(); ++i) {
            auto *c = child(i);
            if (c && !c->state().invisible && c->rect().contains(x, y)) return c;
        }
        int i = bar()->tabAt(bar()->mapFromGlobal(QPoint(x, y)));
        return i < 0 ? nullptr : child(i);
    }
    QAccessibleInterface *focusChild() const override { return child(bar()->currentIndex()); }
    void *interface_cast(QAccessible::InterfaceType type) override {
        if (type == QAccessible::SelectionInterface) return static_cast<QAccessibleSelectionInterface *>(this);
        return QAccessibleWidget::interface_cast(type);
    }
    int selectedItemCount() const override { return bar()->currentIndex() < 0 ? 0 : 1; }
    QList<QAccessibleInterface *> selectedItems() const override {
        auto *tab = child(bar()->currentIndex()); return tab ? QList<QAccessibleInterface *>{tab} : QList<QAccessibleInterface *>{};
    }
    bool select(QAccessibleInterface *item) override {
        int i = indexOfChild(item);
        if (i < 0 || i >= bar()->count() || !bar()->isTabEnabled(i) || !bar()->isTabVisible(i)) return false;
        bar()->setCurrentIndex(i); return true;
    }
    bool unselect(QAccessibleInterface *) override { return false; }
    bool selectAll() override { return false; }
    bool clear() override { return false; }
};

QAccessibleInterface *factory(const QString &name, QObject *object)
{
    if (name == QLatin1String("QTabBar")) return new AccessibleTabBar(static_cast<QTabBar *>(object));
    return nullptr;
}
}

void installAccessibleTabs()
{
    static const bool installed = [] { QAccessible::installFactory(factory); return true; }();
    Q_UNUSED(installed);
}
