#include "native_keyboard.hpp"

#include <QtCore/QEvent>
#include <QtCore/QPointer>
#include <QtCore/QVariant>
#include <QtGui/QApplication>
#include <QtGui/QDesktopWidget>
#include <QtGui/QFocusEvent>
#include <QtGui/QLayout>
#include <QtGui/QScrollArea>

namespace NativeKeyboard {
namespace {

const int watchIntervalMs = 150;
const int editorClearance = 8;

QWidget* layoutAnchor(QWidget* editor) {
    QWidget* window = editor->window();
    for (QWidget* parent = editor->parentWidget(); parent && parent != window; parent = parent->parentWidget()) {
        if (parent->property(LayoutAnchor).toBool())
            return parent;
    }
    return window;
}

void revealEditor(QWidget* editor) {
    for (QWidget* parent = editor->parentWidget(); parent; parent = parent->parentWidget()) {
        if (auto* scroll = qobject_cast<QScrollArea*>(parent)) {
            if (scroll->widget() && scroll->widget()->isAncestorOf(editor))
                scroll->ensureWidgetVisible(editor, 0, editorClearance);
        }
        if (parent->isWindow())
            break;
    }
}

class EditorWatch final : public QObject {
public:
    explicit EditorWatch(QApplication& application) : QObject(&application) {
        application.installEventFilter(this);
    }

protected:
    bool eventFilter(QObject* watched, QEvent* event) override {
        const auto type = event->type();
        if (type == QEvent::FocusIn && watched == QApplication::focusWidget()) {
            changeEditor(static_cast<QWidget*>(watched), static_cast<QFocusEvent*>(event)->reason());
        } else if (watched == editor_.data()) {
            if (type == QEvent::RequestSoftwareInputPanel || type == QEvent::CloseSoftwareInputPanel) {
                if (type == QEvent::CloseSoftwareInputPanel && panelOpen_ && QApplication::focusWidget() &&
                    QApplication::focusWidget() != editor_ && (QApplication::mouseButtons() & Qt::LeftButton)) {
                    stopOnRelease_ = true;
                    return false;
                }
                takePanelEvent();
                touchActive_ = touchInputActive();
                panelOpen_ = type == QEvent::RequestSoftwareInputPanel;
                updateLayout();
            } else if (type == QEvent::Hide) {
                stop(nullptr);
            }
        }
        return false;
    }

    void timerEvent(QTimerEvent* event) override {
        if (event->timerId() != timer_)
            return;
        if (!editor_ || !editor_->isVisible() || !editor_->isEnabled()) {
            stop(nullptr);
            return;
        }
        if (stopOnRelease_) {
            if (!(QApplication::mouseButtons() & Qt::LeftButton))
                stop(QApplication::focusWidget());
            return;
        }
        const PanelEvent notice = takePanelEvent();
        const int active = touchInputActive();
        if (notice != PanelEvent::None)
            panelOpen_ = notice == PanelEvent::Open;
        else if (active >= 0 && active != touchActive_ && (active == 1 || touchActive_ == 1))
            panelOpen_ = active != 0;
        if (active >= 0)
            touchActive_ = active;
        updateLayout();
    }

private:
    void changeEditor(QWidget* focus, Qt::FocusReason reason) {
        stopOnRelease_ = false;
        if (focus == editor_.data())
            return;
        if (!focus->isEnabled() || !focus->testAttribute(Qt::WA_InputMethodEnabled) || fullScreenPreferred()) {
            // Leave the button under the pointer until Qt handles its release.
            if (panelOpen_ && reason == Qt::MouseFocusReason)
                stopOnRelease_ = true;
            else
                stop(focus);
            return;
        }
        if (!setSplitMode(focus, true))
            return;
        editor_ = focus;
        if (!timer_) {
            takePanelEvent();
            touchActive_ = touchInputActive();
            timer_ = startTimer(watchIntervalMs);
        }
        updateLayout();
    }

    void stop(QWidget* focus) {
        if (!timer_)
            return;
        killTimer(timer_);
        timer_ = 0;
        stopOnRelease_ = false;
        editor_ = nullptr;
        panelOpen_ = false;
        restoreLayout();
        setSplitMode(focus, false);
    }

    void restoreLayout() {
        if (layout_ && layout_->contentsMargins() != margins_)
            layout_->setContentsMargins(margins_);
        if (dialog_ && dialog_->pos() != dialogPos_)
            dialog_->move(dialogPos_);
        layout_ = nullptr;
        dialog_ = nullptr;
        revealed_ = nullptr;
    }

    void updateLayout() {
        if (!panelOpen_ || !editor_) {
            restoreLayout();
            return;
        }
        QWidget* anchor = layoutAnchor(editor_);
        QLayout* layout = anchor->layout();
        if (!layout) {
            restoreLayout();
            return;
        }
        const QRect screen = QApplication::desktop()->screenGeometry(anchor);
        const int visibleBottom = splitViewBottom(screen);
        if (visibleBottom < 0) {
            restoreLayout();
            return;
        }
        if (layout_ != layout) {
            restoreLayout();
            layout_ = layout;
            margins_ = layout->contentsMargins();
            if (anchor->windowType() == Qt::Dialog) {
                dialog_ = anchor;
                dialogPos_ = anchor->pos();
            }
        }

        if (dialog_) {
            const int normalBottom = dialogPos_.y() - dialog_->pos().y() + dialog_->mapToGlobal(QPoint(0, dialog_->height())).y();
            const int y = qMax(screen.top(), dialogPos_.y() - qMax(0, normalBottom - visibleBottom));
            if (dialog_->pos().y() != y)
                dialog_->move(dialogPos_.x(), y);
        }

        const QMargins current = layout->contentsMargins();
        const int overlap = anchor->mapToGlobal(QPoint(0, anchor->height())).y() - visibleBottom;
        const int maxInset = anchor->height() - layout->minimumSize().height() + current.bottom() - margins_.bottom();
        const int bottom = margins_.bottom() + qBound(0, overlap, qMax(0, maxInset));
        const bool reflow = current.bottom() != bottom;
        if (reflow) {
            layout->setContentsMargins(margins_.left(), margins_.top(), margins_.right(), bottom);
            layout->activate();
        }
        if (reflow || revealed_ != editor_) {
            revealEditor(editor_);
            revealed_ = editor_;
        }
    }

    QPointer<QWidget> editor_;
    QPointer<QWidget> revealed_;
    QPointer<QLayout> layout_;
    QPointer<QWidget> dialog_;
    QMargins margins_;
    QPoint dialogPos_;
    int timer_ = 0;
    int touchActive_ = -1;
    bool panelOpen_ = false;
    bool stopOnRelease_ = false;
};

} // namespace

void watchEditors(QApplication& application) {
    new EditorWatch(application);
}

} // namespace NativeKeyboard
