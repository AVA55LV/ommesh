#ifdef __SYMBIAN32__
#undef __SYMBIAN_STDCPP_SUPPORT__
#endif

#include "native_keyboard.hpp"

#include <QtCore/QSettings>

#ifdef Q_OS_SYMBIAN
#include <QtGui/QInputContext>
#include <QtGui/QSymbianEvent>
#include <QtGui/QWidget>
#include <e32property.h>
#include <eikappui.h>
#include <eikenv.h>
#endif

namespace NativeKeyboard {

#ifdef Q_OS_SYMBIAN
namespace {

// Avkon resource changes recognized by Symbian Qt's split-view input context.
const int splitViewOpenEvent = 0x2001E2C0;
const int splitViewCloseEvent = 0x2001E2C1;

} // namespace

Application::Application(int& argc, char** argv) : QApplication(argc, argv) {
    watchEditors(*this);
}

bool Application::symbianEventFilter(const QSymbianEvent* event) {
    if (event->type() == QSymbianEvent::ResourceChangeEvent) {
        if (event->resourceChangeType() == splitViewOpenEvent)
            pendingEvent_ = PanelEvent::Open;
        else if (event->resourceChangeType() == splitViewCloseEvent)
            pendingEvent_ = PanelEvent::Closed;
    }
    return false;
}
#endif

bool fullScreenPreferred() {
    return QSettings().value(QLatin1String(FullScreenPreference), false).toBool();
}

bool setSplitMode(QWidget* focusedEditor, bool split) {
#ifdef Q_OS_SYMBIAN
    const Qt::ApplicationAttribute setting = Qt::AA_S60DisablePartialScreenInputMode;
    if (QApplication::testAttribute(setting) != !split) {
        QApplication::setAttribute(setting, !split);
        // Focus has already reached Qt's FEP, so apply the new hint before it opens.
        if (focusedEditor && focusedEditor->testAttribute(Qt::WA_InputMethodEnabled)) {
            if (QInputContext* context = focusedEditor->inputContext())
                context->update();
        }
    }
    return split;
#else
    Q_UNUSED(focusedEditor);
    Q_UNUSED(split);
    return false;
#endif
}

PanelEvent takePanelEvent() {
#ifdef Q_OS_SYMBIAN
    if (auto* app = static_cast<Application*>(QCoreApplication::instance())) {
        const PanelEvent event = app->pendingEvent_;
        app->pendingEvent_ = PanelEvent::None;
        return event;
    }
#endif
    return PanelEvent::None;
}

int touchInputActive() {
#ifdef Q_OS_SYMBIAN
    // The Symbian Qt input context uses this Avkon property to detect the input panel.
    const TUid fep = {0x100056de};
    const TUint activeKey = 4;
    TInt active = 0;
    if (RProperty::Get(fep, activeKey, active) == KErrNone)
        return active != 0;
#endif
    return -1;
}

int splitViewBottom(const QRect& screen) {
    int clientBottom = -1;
#ifdef Q_OS_SYMBIAN
    const CEikonEnv* environment = CEikonEnv::Static();
    const CEikAppUi* application = environment ? environment->EikAppUi() : nullptr;
    if (application) {
        const TRect clientRect = application->ClientRect();
        // Some Belle layouts report the ordinary app area while the panel is open.
        if (clientRect.Height() > 0)
            clientBottom = clientRect.iBr.iY;
    }
#endif
    return splitViewBottomForClient(screen, clientBottom);
}

} // namespace NativeKeyboard
