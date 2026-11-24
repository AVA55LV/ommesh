#include "native_keyboard.hpp"
#include "custom_keyboard.hpp"

#include <QtCore/QEvent>
#include <QtCore/QPointer>
#include <QtCore/QSettings>
#include <QtCore/QTimer>
#include <QtCore/QVariant>
#include <QtGui/QApplication>
#include <QtGui/QDesktopWidget>
#include <QtGui/QFocusEvent>
#include <QtGui/QKeyEvent>
#include <QtGui/QLayout>
#include <QtGui/QLineEdit>
#include <QtGui/QPlainTextEdit>
#include <QtGui/QScrollArea>
#include <QtGui/QTextEdit>

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

bool isEditableText(QWidget* widget) {
    if (auto* line = qobject_cast<QLineEdit*>(widget))
        return !line->isReadOnly();
    if (auto* text = qobject_cast<QTextEdit*>(widget))
        return !text->isReadOnly();
    if (auto* text = qobject_cast<QPlainTextEdit*>(widget))
        return !text->isReadOnly();
    return false;
}

} // namespace

class EditorWatch : public QObject {
    Q_OBJECT

public:
    explicit EditorWatch(QApplication& application) : QObject(&application) {
        application.installEventFilter(this);
    }

protected:
    bool eventFilter(QObject* watched, QEvent* event) override {
        const auto type = event->type();
        if (type == QEvent::FocusIn && watched == QApplication::focusWidget()) {
            changeEditor(static_cast<QWidget*>(watched), static_cast<QFocusEvent*>(event)->reason());
        } else if (customMode_ && customEvent(watched, event)) {
            return true;
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
        if (!editor_ || !editor_->isVisible() || !editor_->isEnabled() ||
            (customMode_ && !isEditableText(editor_))) {
            stop(nullptr);
            return;
        }
        if (stopOnRelease_) {
            if (!(QApplication::mouseButtons() & Qt::LeftButton))
                stop(QApplication::focusWidget());
            return;
        }
        if (customMode_) {
            if (panelOpen_)
                updateLayout();
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

private slots:
    void openCustomPanel() {
        if (!customMode_ || !editor_ || QApplication::focusWidget() != editor_ ||
            !editor_->isVisible() || !editor_->isEnabled() || !isEditableText(editor_))
            return;
        QWidget* window = editor_->window();
        if (!keyboard_ || keyboard_->parentWidget() != window) {
            if (keyboard_) {
                keyboard_->hide();
                keyboard_->deleteLater();
            }
            keyboard_ = new CustomKeyboard(window);
            keyboard_->setObjectName(QLatin1String("customKeyboard"));
            connect(keyboard_, SIGNAL(textEntered(QString)), this, SLOT(enterText(QString)));
            connect(keyboard_, SIGNAL(keyEntered(int)), this, SLOT(enterKey(int)));
            connect(keyboard_, SIGNAL(hideRequested()), this, SLOT(hideCustomPanel()));
        }
        const QSettings prefs;
        const auto hints = editor_->inputMethodHints();
        const bool numeric = hints & (Qt::ImhDigitsOnly | Qt::ImhFormattedNumbersOnly | Qt::ImhDialableCharactersOnly);
        const bool multiline = qobject_cast<QLineEdit*>(editor_) == nullptr;
        keyboard_->configure(prefs.value(QLatin1String(CustomThemePreference), QLatin1String("graphite")).toString(),
                             prefs.value(QLatin1String(CustomVibrationPreference), true).toBool(), numeric, multiline);
        panelOpen_ = true;
        updateLayout();
    }

    void hideCustomPanel() {
        if (!customMode_)
            return;
        panelOpen_ = false;
        restoreLayout();
    }

    void enterText(const QString& text) {
        sendKey(0, text);
    }

    void enterKey(int key) {
        sendKey(key, key == Qt::Key_Return ? QString(QLatin1Char('\r')) : QString());
    }

private:
    bool customEvent(QObject* watched, QEvent* event) {
        if (!editor_)
            return false;
        const auto type = event->type();
        if (type == QEvent::ApplicationDeactivate) {
            stop(nullptr);
            return false;
        }
        if (type == QEvent::KeyPress && panelOpen_) {
            const int key = static_cast<QKeyEvent*>(event)->key();
            if (key == Qt::Key_Escape || key == Qt::Key_Back) {
                hideCustomPanel();
                return true;
            }
        }
        if (type == QEvent::MouseButtonRelease && !panelOpen_ && watched->isWidgetType() &&
            (watched == editor_.data() || editor_->isAncestorOf(static_cast<QWidget*>(watched))) &&
            static_cast<QMouseEvent*>(event)->button() == Qt::LeftButton) {
            QTimer::singleShot(0, this, SLOT(openCustomPanel()));
        }
        if (watched != editor_.data())
            return false;
        if (type == QEvent::RequestSoftwareInputPanel) {
            if (!(QApplication::mouseButtons() & Qt::LeftButton))
                QTimer::singleShot(0, this, SLOT(openCustomPanel()));
            return true;
        }
        if (type == QEvent::CloseSoftwareInputPanel)
            return true;
        return false;
    }

    void sendKey(int key, const QString& text) {
        if (!customMode_ || !panelOpen_ || !editor_ || QApplication::focusWidget() != editor_ ||
            !editor_->isEnabled() || !isEditableText(editor_))
            return;
        const QPointer<QWidget> target(editor_);
        QKeyEvent press(QEvent::KeyPress, key, Qt::NoModifier, text);
        QApplication::sendEvent(target, &press);
        if (target) {
            QKeyEvent release(QEvent::KeyRelease, key, Qt::NoModifier, text);
            QApplication::sendEvent(target, &release);
        }
    }

    void changeEditor(QWidget* focus, Qt::FocusReason reason) {
        stopOnRelease_ = false;
        if (focus == editor_.data())
            return;
        // Qt's item views also advertise input methods before their focus handler runs.
        if (!focus->isEnabled() || !focus->testAttribute(Qt::WA_InputMethodEnabled) || !isEditableText(focus)) {
            // Leave the button under the pointer until Qt handles its release.
            if (panelOpen_ && reason == Qt::MouseFocusReason)
                stopOnRelease_ = true;
            else
                stop(focus);
            return;
        }
        if (QSettings().value(QLatin1String(CustomEnabledPreference), false).toBool()) {
            const bool keepOpen = panelOpen_ && customMode_ && editor_ && focus->window() == editor_->window();
            if (!keepOpen)
                stop(nullptr);
            else
                releaseCustomEditor();
            customMode_ = true;
            editor_ = focus;
            focus->setAttribute(Qt::WA_InputMethodEnabled, false);
            startMonitoring();
            if (keepOpen || (reason != Qt::MouseFocusReason && reason != Qt::ActiveWindowFocusReason))
                QTimer::singleShot(0, this, SLOT(openCustomPanel()));
            return;
        }
        const bool fullScreen = fullScreenPreferred();
        if (customMode_ || fullScreen)
            stop(focus);
        if (fullScreen)
            return;
        if (!setSplitMode(focus, true))
            return;
        editor_ = focus;
        startMonitoring();
        updateLayout();
    }

    void startMonitoring() {
        if (!timer_) {
            takePanelEvent();
            touchActive_ = touchInputActive();
            timer_ = startTimer(watchIntervalMs);
        }
    }

    void releaseCustomEditor() {
        if (customMode_ && editor_ && isEditableText(editor_))
            editor_->setAttribute(Qt::WA_InputMethodEnabled, true);
    }

    void stop(QWidget* focus) {
        if (!timer_)
            return;
        killTimer(timer_);
        timer_ = 0;
        stopOnRelease_ = false;
        releaseCustomEditor();
        customMode_ = false;
        editor_ = nullptr;
        panelOpen_ = false;
        restoreLayout();
        setSplitMode(focus, false);
    }

    void restoreLayout() {
        if (keyboard_)
            keyboard_->hide();
        if (keyboardWindow_) {
            keyboardWindow_->setFocusPolicy(windowFocusPolicy_);
            keyboardWindow_ = nullptr;
        }
        if (layout_ && layout_->contentsMargins() != margins_)
            layout_->setContentsMargins(margins_);
        if (dialog_) {
            if (dialog_->geometry() != dialogGeometry_)
                dialog_->setGeometry(dialogGeometry_);
        }
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
        const int visibleBottom = customMode_ ? customPanelTop(anchor->window(), screen) : splitViewBottom(screen);
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
                dialogGeometry_ = anchor->geometry();
            }
        }

        if (dialog_) {
            const int normalBottom = dialogGeometry_.y() - dialog_->geometry().y() +
                                     dialog_->mapToGlobal(QPoint(0, dialogGeometry_.height())).y();
            const int y = qMax(screen.top(), dialogGeometry_.y() - qMax(0, normalBottom - visibleBottom));
            if (customMode_) {
                const QRect room(dialogGeometry_.x(), y, dialogGeometry_.width(), screen.bottom() + 1 - y);
                if (dialog_->geometry() != room)
                    dialog_->setGeometry(room);
            } else if (dialog_->geometry().y() != y) {
                dialog_->setGeometry(dialogGeometry_.x(), y, dialogGeometry_.width(), dialogGeometry_.height());
            }
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
        if (customMode_ && keyboard_) {
            QWidget* window = anchor->window();
            const int top = window->mapFromGlobal(QPoint(0, visibleBottom)).y();
            const QRect keyboardRect(0, top, window->width(), window->height() - top);
            if (keyboard_->geometry() != keyboardRect)
                keyboard_->setGeometry(keyboardRect);
            if (!keyboard_->isVisible()) {
                // Qt tries click focus on the keyboard's ancestors even though its keys take no focus.
                const auto policy = window->focusPolicy();
                if (policy & Qt::ClickFocus) {
                    keyboardWindow_ = window;
                    windowFocusPolicy_ = policy;
                    window->setFocusPolicy(policy & Qt::TabFocus ? Qt::TabFocus : Qt::NoFocus);
                }
                keyboard_->raise();
                keyboard_->show();
            }
        }
    }

    int customPanelTop(QWidget* window, const QRect& screen) const {
        if (!keyboard_ || screen.isEmpty())
            return -1;
        const int height = qMin(keyboard_->sizeHint().height(), screen.height() * 3 / 5);
        const int bottom = window->windowType() == Qt::Dialog ? screen.bottom() + 1 :
                           window->mapToGlobal(QPoint(0, window->height())).y();
        return bottom - height;
    }

    QPointer<QWidget> editor_;
    QPointer<QWidget> revealed_;
    QPointer<QLayout> layout_;
    QPointer<QWidget> dialog_;
    QPointer<CustomKeyboard> keyboard_;
    QPointer<QWidget> keyboardWindow_;
    Qt::FocusPolicy windowFocusPolicy_ = Qt::NoFocus;
    QMargins margins_;
    QRect dialogGeometry_;
    int timer_ = 0;
    int touchActive_ = -1;
    bool panelOpen_ = false;
    bool stopOnRelease_ = false;
    bool customMode_ = false;
};

void watchEditors(QApplication& application) {
    new EditorWatch(application);
}

} // namespace NativeKeyboard

#include "keyboard_layout.moc"
