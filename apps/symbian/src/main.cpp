#undef __SYMBIAN_STDCPP_SUPPORT__
#include "client.hpp"
#include "window.hpp"

#include <QtGui/QApplication>
#include <e32base.h>
#include <e32debug.h>
#include <f32file.h>

namespace {

class StartupTrace {
public:
    StartupTrace() {
        if (server_.Connect() == KErrNone) {
            server_.MkDirAll(_L("C:\\Data\\"));
            fileOpen_ = file_.Replace(server_, _L("C:\\Data\\OMMeshBelle-startup.txt"),
                                      EFileShareAny | EFileWrite) == KErrNone;
        }
    }

    ~StartupTrace() {
        if (fileOpen_) {
            file_.Close();
        }
        server_.Close();
    }

    void mark(const TDesC8& stage) {
        TBuf<96> debug;
        debug.Copy(stage.Left(debug.MaxLength()));
        RDebug::Print(_L("%S"), &debug);
        if (fileOpen_) {
            file_.Write(stage);
            file_.Write(_L8("\r\n"));
            file_.Flush();
        }
    }

    void markConnection(const char* step, int value) {
        TBuf8<96> line;
        const TPtrC8 text(reinterpret_cast<const TUint8*>(step));
        line.Copy(text.Left(80));
        if (value != Client::TraceWithoutValue) {
            line.Append(_L8(" "));
            line.AppendNum(value);
        }
        mark(line);
    }

private:
    RFs server_;
    RFile file_;
    bool fileOpen_{false};
};

int runApplication(StartupTrace& trace) {
    char name[] = "ommesh";
    char styleOption[] = "-style";
    char style[] = "plastique";
    char* argv[] = {name, styleOption, style, nullptr};
    int argc = 3;
#ifdef Q_OS_SYMBIAN
    QApplication::setAttribute(Qt::AA_S60DontConstructApplicationPanes);
    QApplication::setAttribute(Qt::AA_S60DisablePartialScreenInputMode);
    QApplication::setGraphicsSystem(QString::fromLatin1("raster"));
#endif
    trace.mark(_L8("OMMesh: constructing QApplication"));
    QApplication app(argc, argv);
    trace.mark(_L8("OMMesh: QApplication constructed"));
    QApplication::setOrganizationName(QString::fromLatin1("OMMeshBelle"));
    QApplication::setApplicationName(QString::fromLatin1("OMMesh"));
    QApplication::setApplicationVersion(QString::fromLatin1("0.2.25"));

#ifdef Q_OS_SYMBIAN
    // This firmware callback runs an exit animation that some Nokia ROMs and
    // the emulator cannot execute for a full-screen standalone window.
    QObject::disconnect(&app, SIGNAL(aboutToQuit()), nullptr, nullptr);
    QApplication::setEffectEnabled(Qt::UI_General, false);
#endif
    trace.mark(_L8("OMMesh: constructing Client"));
    Client client(nullptr, [&trace](const char* step, int value) { trace.markConnection(step, value); });
    trace.mark(_L8("OMMesh: constructing ClientWindow"));
    ClientWindow window(client);
#ifdef Q_OS_SYMBIAN
    // Opening the application should not immediately activate the input panel.
    window.setFocusPolicy(Qt::StrongFocus);
    window.setFocus(Qt::OtherFocusReason);
    window.showFullScreen();
    window.setFocus(Qt::OtherFocusReason);
#else
    window.show();
#endif
    trace.mark(_L8("OMMesh: entering event loop"));
    return app.exec();
}

}  // namespace

int main() {
    StartupTrace trace;
    trace.mark(_L8("OMMesh 0.2.25: main entered"));
    TInt result = KErrNone;
    TRAPD(error, result = runApplication(trace));
    if (error != KErrNone) {
        TBuf8<64> failure(_L8("OMMesh: Qt error "));
        failure.AppendNum(error);
        trace.mark(failure);
        TBuf<48> message;
        message.Format(_L("OMMesh error %d"), error);
        User::InfoPrint(message);
        return error;
    }
    TBuf8<40> exitStatus(_L8("OMMesh: event loop exited "));
    exitStatus.AppendNum(result);
    trace.mark(exitStatus);
    return result;
}
