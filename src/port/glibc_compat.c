/* Linux release builds: keeps the executable loadable on glibc 2.34+ (docs/LINUX.md).
 *
 * Built on a newer distribution, the executable would import a few functions that only exist
 * in recent glibc releases: the C23 variants of strtol/sscanf (2.38; glibc headers redirect
 * the plain names to them, also inside the prebuilt Dawn library), strlcpy & co. (2.38; used
 * by SDL when present) and pidfd_spawnp/pidfd_getpid (2.39; weak references in Rust's
 * standard library, part of nod). Defining them here satisfies those references inside the
 * executable, so the dynamic loader never asks libc for the newer symbol versions.
 *
 * This file is C without _GNU_SOURCE / C23 so that the plain names below are the classic
 * glibc functions (no redirection back to the __isoc23_ ones). The only behavioural
 * difference of the C23 variants is accepting "0b" binary prefixes, which nothing here parses.
 */
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <sys/types.h>
#include <wchar.h>

#if defined(__linux__) && defined(__GLIBC__)

#define CF_COMPAT __attribute__((visibility("default"), used))

CF_COMPAT long __isoc23_strtol(const char* s, char** end, int base) { return strtol(s, end, base); }
CF_COMPAT long long __isoc23_strtoll(const char* s, char** end, int base) { return strtoll(s, end, base); }
CF_COMPAT unsigned long __isoc23_strtoul(const char* s, char** end, int base) { return strtoul(s, end, base); }
CF_COMPAT unsigned long long __isoc23_strtoull(const char* s, char** end, int base) { return strtoull(s, end, base); }
CF_COMPAT long __isoc23_wcstol(const wchar_t* s, wchar_t** end, int base) { return wcstol(s, end, base); }
CF_COMPAT unsigned long __isoc23_wcstoul(const wchar_t* s, wchar_t** end, int base) { return wcstoul(s, end, base); }
CF_COMPAT long long __isoc23_wcstoll(const wchar_t* s, wchar_t** end, int base) { return wcstoll(s, end, base); }
CF_COMPAT unsigned long long __isoc23_wcstoull(const wchar_t* s, wchar_t** end, int base) { return wcstoull(s, end, base); }

CF_COMPAT int __isoc23_vsscanf(const char* s, const char* fmt, va_list ap) { return vsscanf(s, fmt, ap); }
CF_COMPAT int __isoc23_vfscanf(FILE* f, const char* fmt, va_list ap) { return vfscanf(f, fmt, ap); }
CF_COMPAT int __isoc23_sscanf(const char* s, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    const int r = vsscanf(s, fmt, ap);
    va_end(ap);
    return r;
}
CF_COMPAT int __isoc23_fscanf(FILE* f, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    const int r = vfscanf(f, fmt, ap);
    va_end(ap);
    return r;
}
CF_COMPAT int __isoc23_scanf(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    const int r = vfscanf(stdin, fmt, ap);
    va_end(ap);
    return r;
}

CF_COMPAT size_t strlcpy(char* dst, const char* src, size_t size) {
    const size_t len = strlen(src);
    if (size) {
        const size_t n = len < size - 1 ? len : size - 1;
        memcpy(dst, src, n);
        dst[n] = '\0';
    }
    return len;
}
CF_COMPAT size_t strlcat(char* dst, const char* src, size_t size) {
    size_t dlen = 0;
    while (dlen < size && dst[dlen]) dlen++;
    if (dlen == size) return size + strlen(src);
    return dlen + strlcpy(dst + dlen, src, size - dlen);
}
CF_COMPAT size_t wcslcpy(wchar_t* dst, const wchar_t* src, size_t size) {
    const size_t len = wcslen(src);
    if (size) {
        const size_t n = len < size - 1 ? len : size - 1;
        wmemcpy(dst, src, n);
        dst[n] = L'\0';
    }
    return len;
}
CF_COMPAT size_t wcslcat(wchar_t* dst, const wchar_t* src, size_t size) {
    size_t dlen = 0;
    while (dlen < size && dst[dlen]) dlen++;
    if (dlen == size) return size + wcslen(src);
    return dlen + wcslcpy(dst + dlen, src, size - dlen);
}

/* Rust's std spawns processes through pidfd_spawnp only when a pidfd is requested (nod never
 * spawns); report "not supported" so it falls back like on older glibc. */
CF_COMPAT int pidfd_spawnp(int* pidfd, const char* file, const void* fa, const void* attr, char* const argv[],
                           char* const envp[]) {
    (void)pidfd, (void)file, (void)fa, (void)attr, (void)argv, (void)envp;
    return ENOSYS;
}
CF_COMPAT pid_t pidfd_getpid(int fd) {
    (void)fd;
    errno = ENOSYS;
    return -1;
}

/* arc4random (2.36): the static C++ runtime's std::random_device uses it when the build
 * host's glibc has it. getrandom() exists since glibc 2.25. */
CF_COMPAT uint32_t arc4random(void) {
    uint32_t v = 0;
    while (getrandom(&v, sizeof(v), 0) != (ssize_t)sizeof(v)) {
        if (errno != EINTR && errno != EAGAIN) break;
    }
    return v;
}
CF_COMPAT void arc4random_buf(void* buf, size_t n) {
    unsigned char* p = (unsigned char*)buf;
    while (n) {
        const ssize_t r = getrandom(p, n, 0);
        if (r <= 0) {
            if (errno == EINTR || errno == EAGAIN) continue;
            break;
        }
        p += r;
        n -= (size_t)r;
    }
}
CF_COMPAT uint32_t arc4random_uniform(uint32_t bound) {
    if (bound < 2) return 0;
    const uint32_t min = -bound % bound;
    for (;;) {
        const uint32_t r = arc4random();
        if (r >= min) return r % bound;
    }
}

/* hypotf: glibc 2.35 added a new symbol version (hypotf@GLIBC_2.35), which std::hypot(float,
 * float) binds to when built on a newer glibc. Defined here in full, hidden: calls inside the
 * executable bind to it at link time and nothing is imported from libm.
 *
 * Up to 1.1.4 this was a default-visibility wrapper calling hypotf@GLIBC_2.2.5 through the PLT.
 * The executable then exported an unversioned hypotf, and the dynamic loader lets an unversioned
 * definition satisfy a versioned reference, searching the executable first: the PLT entry bound
 * to the wrapper itself, so the first std::hypot(float, float) call was an endless jump loop
 * (issues #2 / #11: the frame interpolation's 2D layout blend calls it when a 2D picture slides,
 * e.g. the title screen after START or the pause screen, and the picture froze on every
 * distribution). Never re-export a libc/libm name from the executable that calls the original
 * (docs/LINUX.md "Portable executable"); tools/package_linux.sh checks it.
 *
 * Same arithmetic as glibc 2.35's hypotf: the squares and their sum are exact or correctly
 * rounded in double, so the float result is correctly rounded. */
__attribute__((visibility("hidden"), used)) float hypotf(float x, float y) {
    if (__builtin_isinf(x) || __builtin_isinf(y)) return __builtin_inff();
    if (__builtin_isnan(x) || __builtin_isnan(y)) return x + y;
    const double dx = x, dy = y;
    return (float)__builtin_sqrt(dx * dx + dy * dy);
}

#endif
