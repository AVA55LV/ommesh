#ifndef OMMESH_BELLE_NATIVE_KEYBOARD_HPP
#define OMMESH_BELLE_NATIVE_KEYBOARD_HPP

#include <QtCore/QRect>

#ifdef Q_OS_SYMBIAN
#include <QtGui/QApplication>
#endif

class QWidget;
class QApplication;

namespace NativeKeyboard {

constexpr char FullScreenPreference[] = "ui/fullScreenKeyboard";
constexpr char CustomEnabledPreference[] = "ui/customKeyboard";
constexpr char CustomThemePreference[] = "ui/customKeyboardTheme";
constexpr char CustomVibrationPreference[] = "ui/customKeyboardVibration";
constexpr char LayoutAnchor[] = "nativeKeyboardLayoutAnchor";

enum class PanelEvent { None, Open, Closed };

#ifdef Q_OS_SYMBIAN
class Application : public QApplication {
public:
    Application(int& argc, char** argv);

protected:
    bool symbianEventFilter(const QSymbianEvent* event) override;

private:
    friend PanelEvent takePanelEvent();
    PanelEvent pendingEvent_ = PanelEvent::None;
};
#endif

bool fullScreenPreferred();
void watchEditors(QApplication& application);
bool setSplitMode(QWidget* focusedEditor, bool split);
PanelEvent takePanelEvent();
int touchInputActive();
int splitViewBottom(const QRect& screen);

inline int splitViewBottomForClient(const QRect& screen, int clientBottom) {
    if (screen.isEmpty())
        return -1;
    // Ordinary status and softkey panes are much smaller than the input panel.
    if (clientBottom > screen.top() && screen.bottom() + 1 - clientBottom >= screen.height() / 4)
        return clientBottom;
    // Nokia's portrait FEP can cover about half of the screen.
    return screen.top() + screen.height() / 2;
}

} // namespace NativeKeyboard

#endif
