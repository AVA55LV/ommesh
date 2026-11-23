#ifndef OMMESH_BELLE_NATIVE_VIBRATION_HPP
#define OMMESH_BELLE_NATIVE_VIBRATION_HPP

class CHWRMVibra;

class NativeVibration {
public:
    NativeVibration() = default;
    NativeVibration(const NativeVibration&) = delete;
    NativeVibration& operator=(const NativeVibration&) = delete;
    ~NativeVibration();

    int pulse();

private:
    CHWRMVibra* motor_ = nullptr;
};

#endif
