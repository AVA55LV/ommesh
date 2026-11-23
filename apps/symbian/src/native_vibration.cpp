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

int NativeVibration::pulse() {
#ifdef __SYMBIAN32__
    TRAPD(error, {
        if (!motor_)
            motor_ = CHWRMVibra::NewL();
        motor_->StartVibraL(450);
    });
    return error;
#else
    return -5;
#endif
}
