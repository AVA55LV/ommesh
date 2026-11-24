#ifdef __SYMBIAN32__
#undef __SYMBIAN_STDCPP_SUPPORT__
#endif

#include "native_location.hpp"

#ifdef __SYMBIAN32__
#include <e32base.h>
#include <lbs.h>
#include <cmath>

namespace {

const int kPositionTimeoutMs = 120000;

bool isIntegratedGps(const TPositionModuleInfo& module) {
    return module.IsAvailable() &&
           module.DeviceLocation() == TPositionModuleInfo::EDeviceInternal &&
           module.TechnologyType() == TPositionModuleInfo::ETechnologyTerminal &&
           (module.Capabilities() & TPositionModuleInfo::ECapabilitySatellite) != 0;
}

int gpsModule(RPositionServer& server, TPositionModuleId& moduleId) {
    TUint count = 0;
    const int error = server.GetNumModules(count);
    if (error != KErrNone)
        return error;

    for (TUint index = 0; index < count; ++index) {
        TPositionModuleInfo module;
        if (server.GetModuleInfoByIndex(index, module) == KErrNone && isIntegratedGps(module)) {
            moduleId = module.ModuleId();
            return KErrNone;
        }
    }
    return KErrNotFound;
}

}  // namespace

class NativeLocation::Backend : public CActive {
public:
    explicit Backend(NativeLocation& owner) : CActive(EPriorityStandard), owner_(owner) {
        CActiveScheduler::Add(this);
    }

    ~Backend() override { reset(); }

    int start() {
        const int connectError = server_.Connect();
        if (connectError != KErrNone)
            return connectError;
        serverOpen_ = true;

        TPositionModuleId moduleId;
        const int moduleError = gpsModule(server_, moduleId);
        if (moduleError != KErrNone)
            return moduleError;

        const int openError = positioner_.Open(server_, moduleId);
        if (openError != KErrNone)
            return openError;
        positionerOpen_ = true;

        const int requestorError = positioner_.SetRequestor(
            CRequestor::ERequestorService, CRequestor::EFormatApplication, _L("OMMesh Belle"));
        if (requestorError != KErrNone)
            return requestorError;

        // A per-request timer bounds the wait. Zero max age requests a fresh position.
        const TPositionUpdateOptions options(TTimeIntervalMicroSeconds(0),
            TTimeIntervalMicroSeconds(0), TTimeIntervalMicroSeconds(0), EFalse);
        const int optionsError = positioner_.SetUpdateOptions(options);
        if (optionsError != KErrNone)
            return optionsError;

        info_ = TPositionInfo();
        positioner_.NotifyPositionUpdate(info_, iStatus);
        SetActive();
        return KErrNone;
    }

    void reset() {
        Cancel();
        if (positionerOpen_) {
            positioner_.Close();
            positionerOpen_ = false;
        }
        if (serverOpen_) {
            server_.Close();
            serverOpen_ = false;
        }
    }

private:
    void DoCancel() override {
        positioner_.CancelRequest(RPositioner::EReqNotifyPositionUpdate);
    }

    void RunL() override {
        const int error = iStatus.Int();
        if (error != KErrNone) {
            owner_.complete(error);
            return;
        }

        TPosition position;
        info_.GetPosition(position);
        const double latitude = position.Latitude();
        const double longitude = position.Longitude();
        const double accuracy = position.HorizontalAccuracy();
        if (!std::isfinite(latitude) || !std::isfinite(longitude) ||
                latitude < -90 || latitude > 90 || longitude < -180 || longitude > 180) {
            owner_.complete(KErrCorrupt);
            return;
        }
        owner_.complete(KErrNone, latitude, longitude,
                        std::isfinite(accuracy) && accuracy >= 0 ? accuracy : -1);
    }

    NativeLocation& owner_;
    RPositionServer server_;
    RPositioner positioner_;
    TPositionInfo info_;
    bool serverOpen_ = false;
    bool positionerOpen_ = false;
};
#endif

NativeLocation::NativeLocation(QObject* parent) : QObject(parent) {
    timer_.setSingleShot(true);
    connect(&timer_, SIGNAL(timeout()), this, SLOT(timerElapsed()));
}

NativeLocation::~NativeLocation() {
    cancel();
#ifdef __SYMBIAN32__
    delete backend_;
#endif
}

void NativeLocation::requestFix() {
    if (phase_ != Idle)
        return;
    phase_ = Starting;
    timer_.start(0);
}

void NativeLocation::cancel() {
    timer_.stop();
    phase_ = Idle;
#ifdef __SYMBIAN32__
    if (backend_)
        backend_->reset();
#endif
}

bool NativeLocation::isRequesting() const {
    return phase_ != Idle;
}

void NativeLocation::timerElapsed() {
#ifdef __SYMBIAN32__
    if (phase_ == Waiting) {
        complete(KErrTimedOut);
        return;
    }

    if (!CActiveScheduler::Current()) {
        complete(KErrNotReady);
        return;
    }
    if (!backend_)
        backend_ = new Backend(*this);

    const int error = backend_->start();
    if (error != KErrNone) {
        complete(error);
        return;
    }
    phase_ = Waiting;
    timer_.start(kPositionTimeoutMs);
#else
    complete(-5);  // KErrNotSupported
#endif
}

void NativeLocation::complete(int error, double latitude, double longitude, double horizontalAccuracy) {
    if (!isRequesting())
        return;
    cancel();
    if (error != 0)
        emit fixFailed(error);
    else
        emit fixReady(latitude, longitude, horizontalAccuracy);
}
