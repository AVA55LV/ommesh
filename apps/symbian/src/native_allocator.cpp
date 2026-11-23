#undef __SYMBIAN_STDCPP_SUPPORT__
#include <e32std.h>

namespace {
// The native heap guarantees EABI alignment from eight bytes onward.
constexpr unsigned int kMinimumCell = 8;
}

// Qt can destroy children allocated by this executable with its own
// deleting destructors. Return native heap cells without an SDK header.
extern "C" void* __wrap_SymbianRuntimeAllocate(unsigned int size) {
    if (size > static_cast<unsigned int>(KMaxTInt)) {
        return nullptr;
    }
    return User::Alloc(static_cast<TInt>(size < kMinimumCell ? kMinimumCell : size));
}

extern "C" void __wrap_SymbianRuntimeFree(void* pointer) {
    if (pointer != nullptr) {
        User::Free(pointer);
    }
}

extern "C" void* __wrap_SymbianRuntimeReallocate(void* pointer, unsigned int size) {
    if (pointer == nullptr) {
        return __wrap_SymbianRuntimeAllocate(size);
    }
    if (size == 0) {
        __wrap_SymbianRuntimeFree(pointer);
        return nullptr;
    }
    if (size > static_cast<unsigned int>(KMaxTInt)) {
        return nullptr;
    }
    return User::ReAlloc(pointer, static_cast<TInt>(size < kMinimumCell ? kMinimumCell : size));
}
