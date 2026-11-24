#include "native_vibration.hpp"

#ifdef __SYMBIAN32__
#undef __SYMBIAN_STDCPP_SUPPORT__
#include <e32base.h>
#include <hwrmvibra.h>
#endif

NativeVibration::~NativeVibration() {
#ifdef __SYMBIAN32__
    delete motor_;
#endif
}

int NativeVibration::pulse(int durationMs) {
#ifdef __SYMBIAN32__
    for (int attempt = 0; attempt < 2; ++attempt) {
        TRAPD(error, {
            if (!motor_)
                motor_ = CHWRMVibra::NewL();
            if (motor_->VibraSettings() == CHWRMVibra::EVibraModeOFF)
                User::Leave(KErrAccessDenied);
            motor_->StartVibraL(durationMs);
        });
        if (error != KErrBadHandle)
            return error;
        delete motor_;
        motor_ = nullptr;
    }
    return KErrBadHandle;
#else
    static_cast<void>(durationMs);
    return -5;
#endif
}
