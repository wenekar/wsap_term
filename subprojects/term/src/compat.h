#ifndef WSAP_VTCORE_COMPAT_H
#define WSAP_VTCORE_COMPAT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#if defined(_MSC_VER)
#include <intrin.h>
#endif
#else
#include <pthread.h>
#include <unistd.h>
#endif

#if defined(__GNUC__) || defined(__clang__)
#define WSAP_VTCORE_COMPILER_GNU 1
#elif defined(_MSC_VER)
#define WSAP_VTCORE_COMPILER_MSVC 1
#include <intrin.h>
#endif

#if !defined(WSAP_VTCORE_NO_COMPILER_BUILTINS) && defined(WSAP_VTCORE_COMPILER_GNU)
#define WSAP_VTCORE_HAVE_BUILTINS 1
#elif !defined(WSAP_VTCORE_NO_COMPILER_BUILTINS) && defined(WSAP_VTCORE_COMPILER_MSVC)
#define WSAP_VTCORE_HAVE_MSVC_BITOPS 1
#endif

#if defined(WSAP_VTCORE_COMPILER_GNU)
#define WSAP_VTCORE_HOT __attribute__((hot))
#define WSAP_VTCORE_NOINLINE __attribute__((noinline))
#define WSAP_VTCORE_ALWAYS_INLINE __attribute__((always_inline))
#define WSAP_VTCORE_MAYBE_UNUSED __attribute__((unused))
#elif defined(WSAP_VTCORE_COMPILER_MSVC)
#define WSAP_VTCORE_HOT
#define WSAP_VTCORE_NOINLINE __declspec(noinline)
#define WSAP_VTCORE_ALWAYS_INLINE __forceinline
#define WSAP_VTCORE_MAYBE_UNUSED
#else
#define WSAP_VTCORE_HOT
#define WSAP_VTCORE_NOINLINE
#define WSAP_VTCORE_ALWAYS_INLINE
#define WSAP_VTCORE_MAYBE_UNUSED
#endif

#if defined(WSAP_VTCORE_HAVE_BUILTINS)
#define WSAP_VTCORE_EXPECT(x, v) __builtin_expect((x), (v))
#define WSAP_VTCORE_LIKELY(x) __builtin_expect(!!(x), 1)
#define WSAP_VTCORE_UNLIKELY(x) __builtin_expect(!!(x), 0)
#define WSAP_VTCORE_CONSTANT_P(x) __builtin_constant_p(x)
#else
#define WSAP_VTCORE_EXPECT(x, v) (x)
#define WSAP_VTCORE_LIKELY(x) (x)
#define WSAP_VTCORE_UNLIKELY(x) (x)
#define WSAP_VTCORE_CONSTANT_P(x) 0
#endif

#if defined(WSAP_VTCORE_HAVE_BUILTINS)

static inline int wsap_vtcore_ctz32(uint32_t x) { return x ? __builtin_ctz(x) : 32; }
static inline int wsap_vtcore_clz32(uint32_t x) { return x ? __builtin_clz(x) : 32; }
static inline int wsap_vtcore_popcount32(uint32_t x) { return __builtin_popcount(x); }
static inline int wsap_vtcore_ctz64(uint64_t x) { return x ? __builtin_ctzll(x) : 64; }
static inline int wsap_vtcore_clz64(uint64_t x) { return x ? __builtin_clzll(x) : 64; }
static inline int wsap_vtcore_popcount64(uint64_t x) { return __builtin_popcountll(x); }

#else

#if defined(WSAP_VTCORE_HAVE_MSVC_BITOPS)
static inline int wsap_vtcore_ctz32(uint32_t x)
{
    unsigned long i;
    return _BitScanForward(&i, x) ? (int)i : 32;
}
static inline int wsap_vtcore_clz32(uint32_t x)
{
    unsigned long i;
    return _BitScanReverse(&i, x) ? 31 - (int)i : 32;
}
static inline int wsap_vtcore_popcount32(uint32_t x) { return (int)__popcnt(x); }
#else
static inline int wsap_vtcore_ctz32(uint32_t x)
{
    if (!x)
        return 32;
    static const uint8_t de_bruijn[32] = {
        0, 1, 28, 2, 29, 14, 24, 3, 30, 22, 20, 15, 25, 17, 4, 8,
        31, 27, 13, 23, 21, 19, 16, 7, 26, 12, 18, 6, 11, 5, 10, 9
    };
    return (int)de_bruijn[((uint32_t)((x & -x) * 0x077CB531U)) >> 27];
}
static inline int wsap_vtcore_clz32(uint32_t x)
{
    if (!x)
        return 32;
    int n = 0;
    if (!(x & 0xffff0000u)) { n += 16; x <<= 16; }
    if (!(x & 0xff000000u)) { n += 8;  x <<= 8;  }
    if (!(x & 0xf0000000u)) { n += 4;  x <<= 4;  }
    if (!(x & 0xc0000000u)) { n += 2;  x <<= 2;  }
    if (!(x & 0x80000000u)) { n += 1; }
    return n;
}
static inline int wsap_vtcore_popcount32(uint32_t x)
{
    x = x - ((x >> 1) & 0x55555555u);
    x = (x & 0x33333333u) + ((x >> 2) & 0x33333333u);
    return (int)((((x + (x >> 4)) & 0x0F0F0F0Fu) * 0x01010101u) >> 24);
}
#endif

static inline int wsap_vtcore_ctz64(uint64_t x)
{
    uint32_t lo = (uint32_t)x;
    return lo ? wsap_vtcore_ctz32(lo) : 32 + wsap_vtcore_ctz32((uint32_t)(x >> 32));
}

static inline int wsap_vtcore_clz64(uint64_t x)
{
    uint32_t hi = (uint32_t)(x >> 32);
    return hi ? wsap_vtcore_clz32(hi) : 32 + wsap_vtcore_clz32((uint32_t)x);
}

static inline int wsap_vtcore_popcount64(uint64_t x)
{
    return wsap_vtcore_popcount32((uint32_t)x) + wsap_vtcore_popcount32((uint32_t)(x >> 32));
}

#endif

static inline uint32_t wsap_vtcore_next_pow2_32(uint32_t x)
{
    uint32_t v = x <= 1 ? 1 : x - 1;
    int lz = wsap_vtcore_clz32(v);
    uint32_t res = UINT32_C(1) << ((32 - lz) & 31);
    return (x <= 1) ? 1 : (lz == 0 ? UINT32_MAX : res);
}

static inline uint64_t wsap_vtcore_next_pow2_64(uint64_t x)
{
    uint64_t v = x <= 1 ? 1 : x - 1;
    int lz = wsap_vtcore_clz64(v);
    uint64_t res = UINT64_C(1) << ((64 - lz) & 63);
    return (x <= 1) ? 1 : (lz == 0 ? UINT64_MAX : res);
}

static inline uint32_t wsap_vtcore_grow_cap32(uint32_t cap, uint32_t need)
{
    return cap >= need ? cap : wsap_vtcore_next_pow2_32(need);
}

static inline uint64_t wsap_vtcore_grow_cap64(uint64_t cap, uint64_t need)
{
    return cap >= need ? cap : wsap_vtcore_next_pow2_64(need);
}

static inline size_t wsap_vtcore_grow_cap(size_t cap, size_t need)
{
#if SIZE_MAX > UINT32_MAX
    return (size_t)wsap_vtcore_grow_cap64(cap, need);
#else
    return (size_t)wsap_vtcore_grow_cap32((uint32_t)cap, (uint32_t)need);
#endif
}

static inline void wsap_vtcore_cpu_relax(void)
{
#if defined(WSAP_VTCORE_COMPILER_GNU)
#if defined(WSAP_VTCORE_HAVE_BUILTINS) && defined(__x86_64__)
    __builtin_ia32_pause();
#elif defined(__aarch64__)
    __asm__ __volatile__("yield");
#endif
#elif defined(WSAP_VTCORE_COMPILER_MSVC)
#if defined(_M_X64) || defined(_M_IX86)
    _mm_pause();
#elif defined(_M_ARM64)
    __yield();
#endif
#endif
}

#if defined(_WIN32)
typedef SRWLOCK wsap_vtcore_mutex;
#else
typedef pthread_mutex_t wsap_vtcore_mutex;
#if !defined(PTHREAD_PRIO_INHERIT)
#define PTHREAD_PRIO_INHERIT 1
#endif
#endif

static inline bool wsap_vtcore_mutex_init(wsap_vtcore_mutex *m)
{
#if defined(_WIN32)
    InitializeSRWLock(m);
    return true;
#elif defined(_POSIX_THREAD_PRIO_INHERIT) && _POSIX_THREAD_PRIO_INHERIT > 0
    pthread_mutexattr_t attr;
    if (pthread_mutexattr_init(&attr) != 0)
        return false;
    bool ok = pthread_mutexattr_setprotocol(&attr, PTHREAD_PRIO_INHERIT) == 0 &&
              pthread_mutex_init(m, &attr) == 0;
    pthread_mutexattr_destroy(&attr);
    return ok;
#else
    return pthread_mutex_init(m, NULL) == 0;
#endif
}

static inline void wsap_vtcore_mutex_lock(wsap_vtcore_mutex *m)
{
#if defined(_WIN32)
    AcquireSRWLockExclusive(m);
#else
    (void)pthread_mutex_lock(m);
#endif
}

static inline void wsap_vtcore_mutex_unlock(wsap_vtcore_mutex *m)
{
#if defined(_WIN32)
    ReleaseSRWLockExclusive(m);
#else
    (void)pthread_mutex_unlock(m);
#endif
}

static inline void wsap_vtcore_mutex_destroy(wsap_vtcore_mutex *m)
{
#if defined(_WIN32)
    (void)m;
#else
    (void)pthread_mutex_destroy(m);
#endif
}

static inline char *wsap_vtcore_strdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *copy = malloc(n);
    if (copy)
        memcpy(copy, s, n);
    return copy;
}

static inline bool wsap_vtcore_has_prefix_ci(const char *s, const char *prefix)
{
    // Fast path: safe word comparison for 7-byte file:// prefix.
    if (WSAP_VTCORE_CONSTANT_P(prefix) && strcmp(prefix, "file://") == 0) {
        if (WSAP_VTCORE_EXPECT(((uintptr_t)s & 4095) <= 4096 - 8, 1)) {
            uint32_t head;
            memcpy(&head, s, 4);
            if ((head | 0x20202020u) != 0x656c6966u)
                return false;
            uint32_t tail;
            memcpy(&tail, s + 3, 4);
            return (tail | 0x00000020u) == 0x2f2f3a65u;
        }
    }
    for (; *prefix; prefix++, s++) {
        unsigned char a = (unsigned char)*s;
        unsigned char b = (unsigned char)*prefix;
        a |= ((unsigned)(a - 'A') < 26) ? 0x20 : 0;
        b |= ((unsigned)(b - 'A') < 26) ? 0x20 : 0;
        if (a != b)
            return false;
    }
    return true;
}

static inline void *wsap_vtcore_aligned_alloc(size_t align, size_t size)
{
    if (size > SIZE_MAX - align - sizeof(void *))
        return NULL;
    void *raw = malloc(size + align + sizeof(void *));
    if (!raw)
        return NULL;
    uintptr_t p = ((uintptr_t)raw + sizeof(void *) + align - 1) & ~((uintptr_t)align - 1);
    ((void **)p)[-1] = raw;
    return (void *)p;
}

static inline void wsap_vtcore_aligned_free(void *p)
{
    if (p)
        free(((void **)p)[-1]);
}

#endif
