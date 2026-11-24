#ifdef __SYMBIAN32__
#undef __SYMBIAN_STDCPP_SUPPORT__
#endif

#include "native_keyboard.hpp"

#include <QtCore/QSettings>

#ifdef Q_OS_SYMBIAN
#include <QtGui/QApplication>
#include <QtGui/QInputContext>
#include <QtGui/QWidget>
#include <e32property.h>
#include <eikappui.h>
#include <eikenv.h>
#endif

namespace NativeKeyboard {

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

int splitViewBottom() {
#ifdef Q_OS_SYMBIAN
    // The Symbian Qt input context uses this Avkon property to detect the input panel.
    const TUid fep = {0x100056de};
    const TUint touchInputActive = 4;
    TInt active = 0;
    if (RProperty::Get(fep, touchInputActive, active) != KErrNone || active == 0)
        return -1;
    const CEikonEnv* environment = CEikonEnv::Static();
    const CEikAppUi* application = environment ? environment->EikAppUi() : nullptr;
    if (application) {
        const TRect clientRect = application->ClientRect();
        if (clientRect.Height() > 0)
            return clientRect.iBr.iY;
    }
#endif
    return -1;
}

} // namespace NativeKeyboard
