#ifndef OMMESH_BELLE_NATIVE_VIBRATION_HPP
#define OMMESH_BELLE_NATIVE_VIBRATION_HPP

class CHWRMVibra;

class NativeVibration {
public:
    enum Failure { ProfileDisabled = -21, SystemLocked = -22 };

    NativeVibration() = default;
    NativeVibration(const NativeVibration&) = delete;
    NativeVibration& operator=(const NativeVibration&) = delete;
    ~NativeVibration();

    int pulse(int durationMs = 250);

private:
    CHWRMVibra* motor_ = nullptr;
};

#endif
