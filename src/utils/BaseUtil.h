/* Copyright 2012 the SumatraPDF project authors (see AUTHORS file).
   License: Simplified BSD (see COPYING.BSD) */

#ifndef BaseUtil_h
#define BaseUtil_h

#if defined(_UNICODE) && !defined(UNICODE)
#define UNICODE
#endif
#if defined(UNICODE) && !defined(_UNICODE)
#define _UNICODE
#endif

#include <windows.h>
#include <unknwn.h>
#include <shlwapi.h>
#include <shlobj.h>
#include <gdiplus.h>

#ifdef DEBUG
#define _CRTDBG_MAP_ALLOC
#endif

#include <stdlib.h>
#ifdef DEBUG
#include <crtdbg.h>
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

#ifndef UNICODE
#include <sys/types.h>
#include <sys/stat.h>
#endif
#include <tchar.h>
#include <wchar.h>
#include <string.h>

/* Few most common includes for C stdlib */
#include <assert.h>
#include <float.h>
#include <stddef.h>
#include <stdio.h>
#include <stdint.h>
#include <time.h>
#include <locale.h>

#define _USE_MATH_DEFINES
#include <math.h>

/* Ugly name, but the whole point is to make things shorter.
   SAZA = Struct Allocate and Zero memory for Array
   (note: use operator new for single structs/classes) */
#define SAZA(struct_name, n) (struct_name *)calloc((n), sizeof(struct_name))

#define dimof(X)    (sizeof(X)/sizeof((X)[0]))
#define NoOp()      ((void)0)

/* compile-time assert */
#define STATIC_ASSERT(exp, name) typedef int assert_##name [(exp) != FALSE]

typedef unsigned char uint8;
typedef int16_t   int16;
typedef uint16_t uint16;
typedef int32_t   int32;
typedef uint32_t uint32;
typedef int64_t   int64;
typedef uint64_t uint64;

// useful for setting an 'invalid' state for size_t variables
#define MAX_SIZE_T (size_t)(-1)

STATIC_ASSERT(2 == sizeof(int16),   int16_is_2_bytes);
STATIC_ASSERT(2 == sizeof(uint16), uint16_is_2_bytes);
STATIC_ASSERT(4 == sizeof(int32),   int32_is_4_bytes);
STATIC_ASSERT(4 == sizeof(uint32),  uint32_is_4_bytes);
STATIC_ASSERT(8 == sizeof(int64),   int64_is_8_bytes);
STATIC_ASSERT(8 == sizeof(uint64),  uint64_is_8_bytes);

void CrashMe(); // in StrUtil.cpp

// CrashIf() is like assert() except it crashes in debug and pre-release builds.
// The idea is that assert() indicates "can't possibly happen" situation and if
// it does happen, we would like to fix the underlying cause.
// In practice in our testing we rarely get notified when an assert() is triggered
// and they are disabled in builds running on user's computers.
// Now that we have crash reporting, we can get notified about such cases if we
// use CrashIf() instead of assert(), which we should be doing from now on.
//
// Enabling it in pre-release builds but not in release builds is trade-off between
// shipping small executables (each CrashIf() adds few bytes of code) and having
// more testing on user's machines and not only in our personal testing.
// To crash uncoditionally use CrashAlwaysIf(). It should only be used in
// rare cases where we really want to know a given condition happens. Before
// each release we should audit the uses of CrashAlawysIf()
//
// Just as with assert(), the condition is not guaranteed to be executed
// in some builds, so it shouldn't contain the actual logic of the code
#if defined(SVN_PRE_RELEASE_VER) || defined(DEBUG)
#define CrashIf(exp) \
    { if (exp) \
        CrashMe(); }
#else
#define CrashIf(exp) NoOp()
#endif

#define CrashAlwaysIf(exp) \
    { if (exp) \
        CrashMe(); }

// AssertCrash is like assert() but crashes like CrashIf()
// It's meant to make converting assert() easier (converting to
// CrashIf() requires inverting the condition, which can introduce bugs)
#define AssertCrash(exp) CrashIf(!(exp))

template <typename T>
inline void swap(T& one, T&two)
{
    T tmp = one; one = two; two = tmp;
}

template <typename T>
inline T limitValue(T val, T min, T max)
{
    assert(max >= min);
    if (val < min) return min;
    if (val > max) return max;
    return val;
}

inline void *memdup(const void *data, size_t len)
{
    void *dup = malloc(len);
    if (dup)
        memcpy(dup, data, len);
    return dup;
}
#define _memdup(ptr) memdup(ptr, sizeof(*(ptr)))

inline bool memeq(const void *s1, const void *s2, size_t len)
{
    return 0 == memcmp(s1, s2, len);
}

#include "Allocator.h"
#include "GeomUtil.h"
#include "Scoped.h"
#include "StrUtil.h"
#include "Vec.h"

#endif
