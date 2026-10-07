// Zydek: lets the APK run on Android 9-13 as well as 14+.
//
// Qt (prebuilt for Android 15 in Mixxx's dependency bundle) and Mixxx itself reference two functions
// that older Android doesn't have, so loading them fails ("cannot locate symbol"). This small library
// provides them; the build adds it to the libraries' needed lists (patchelf, see CMakeLists.txt). On
// newer Android the system's own versions come first in the lookup order, so these are only fallbacks.

#include <sys/syscall.h>
#include <unistd.h>

#include <cerrno>

struct APerformanceHintSession;

extern "C" {

/// Android 14: the kernel has had this system call since Linux 4.5, only the libc wrapper is new.
__attribute__((visibility("default"))) ssize_t copy_file_range(int fdIn,
        off64_t* pOffIn,
        int fdOut,
        off64_t* pOffOut,
        size_t length,
        unsigned int flags) {
    return syscall(__NR_copy_file_range, fdIn, pOffIn, fdOut, pOffOut, length, flags);
}

/// Android 15: only a scheduling hint, so "not supported" is a fine answer.
__attribute__((visibility("default"))) int APerformanceHint_setPreferPowerEfficiency(
        APerformanceHintSession* pSession, bool enabled) {
    (void)pSession;
    (void)enabled;
    return ENOTSUP;
}

} // extern "C"
