/*
 * pogls_platform.h — POGLS V3.9  Cross-Platform Compatibility
 * ══════════════════════════════════════════════════════════════════════
 *
 * Handles:
 *   fsync()       Linux/Mac → FlushFileBuffers() Windows
 *   mmap()        Linux/Mac → CreateFileMapping() Windows
 *   atomic ops    GCC built-ins → MSVC interlocked
 *   threading     pthread → Win32 threads
 *
 * Usage: #include "pogls_platform.h" ก่อน header อื่นทุกตัว
 * ══════════════════════════════════════════════════════════════════════
 */
#ifndef POGLS_PLATFORM_H
#define POGLS_PLATFORM_H

/* ── Detect Platform ─────────────────────────────────────────────── */
#if defined(_WIN32) || defined(_WIN64)
  #define POGLS_WINDOWS 1
#elif defined(__linux__)
  #define POGLS_LINUX   1
#elif defined(__APPLE__)
  #define POGLS_MACOS   1
#endif

/* ══════════════════════════════════════════════════════════════════
 * Windows Compatibility Layer
 * ══════════════════════════════════════════════════════════════════ */
#ifdef POGLS_WINDOWS

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <io.h>
#include <direct.h>
#include <stdint.h>
#include <stdio.h>

/* fsync → FlushFileBuffers */
static inline int pogls_fsync(int fd)
{
    HANDLE h = (HANDLE)_get_osfhandle(fd);
    if (h == INVALID_HANDLE_VALUE) return -1;
    return FlushFileBuffers(h) ? 0 : -1;
}
#define fsync(fd)  pogls_fsync(fd)

/* mkdir → _mkdir (Windows single arg) */
#define mkdir(path, mode)  _mkdir(path)

/* usleep → Sleep (ms) */
#define usleep(us)  Sleep((us) / 1000)

/* ssize_t */
typedef long long ssize_t;

/* mmap constants (stub — use pogls_mmap instead) */
#define PROT_READ    0x1
#define PROT_WRITE   0x2
#define MAP_SHARED   0x1
#define MAP_FAILED   ((void*)-1)

/* Windows mmap via CreateFileMapping */
#include <sys/types.h>

static inline void *pogls_mmap(void *addr, size_t length,
                                int prot, int flags,
                                int fd, off_t offset)
{
    (void)addr; (void)flags; (void)offset;
    HANDLE hFile = (HANDLE)_get_osfhandle(fd);
    if (hFile == INVALID_HANDLE_VALUE) return MAP_FAILED;

    DWORD protect = (prot & PROT_WRITE) ? PAGE_READWRITE : PAGE_READONLY;
    HANDLE hMap   = CreateFileMapping(hFile, NULL, protect, 0, 0, NULL);
    if (!hMap) return MAP_FAILED;

    DWORD access  = (prot & PROT_WRITE) ? FILE_MAP_WRITE : FILE_MAP_READ;
    void *ptr     = MapViewOfFile(hMap, access, 0, 0, length);
    CloseHandle(hMap);
    return ptr ? ptr : MAP_FAILED;
}
#define mmap(a,l,p,f,fd,o)  pogls_mmap(a,l,p,f,fd,o)

static inline int pogls_munmap(void *addr, size_t length)
{
    (void)length;
    return UnmapViewOfFile(addr) ? 0 : -1;
}
#define munmap(a,l)  pogls_munmap(a,l)

/* madvise stub (no-op on Windows) */
#define MADV_DONTNEED  0
#define MADV_WILLNEED  0
static inline int madvise(void *a, size_t l, int advice)
{ (void)a;(void)l;(void)advice; return 0; }

/* pthread → Win32 threads minimal shim */
#ifndef _PTHREAD_H
typedef HANDLE pthread_t;
typedef CRITICAL_SECTION pthread_mutex_t;

#define pthread_mutex_init(m,a)    InitializeCriticalSection(m)
#define pthread_mutex_lock(m)      EnterCriticalSection(m)
#define pthread_mutex_unlock(m)    LeaveCriticalSection(m)
#define pthread_mutex_destroy(m)   DeleteCriticalSection(m)

typedef struct { HANDLE h; void*(*fn)(void*); void*arg; } _PthreadCtx;
static DWORD WINAPI _pthread_wrapper(LPVOID p) {
    _PthreadCtx *c = (_PthreadCtx*)p;
    c->fn(c->arg); return 0;
}
static inline int pthread_create(pthread_t *t, void *attr,
                                  void*(*fn)(void*), void *arg) {
    (void)attr;
    _PthreadCtx *c = (_PthreadCtx*)malloc(sizeof(*c));
    c->fn=fn; c->arg=arg;
    *t = CreateThread(NULL,0,_pthread_wrapper,c,0,NULL);
    return *t ? 0 : -1;
}
static inline int pthread_join(pthread_t t, void **r) {
    (void)r; WaitForSingleObject(t,INFINITE); CloseHandle(t); return 0;
}
#endif /* _PTHREAD_H */

/* atomic — use GCC built-ins if MinGW, else MSVC interlocked */
#if !defined(__GNUC__)
  #define __sync_fetch_and_add(p,v)  InterlockedAdd64((LONG64*)(p),(v))
  #define __builtin_expect(x,y)      (x)
  #define __builtin_prefetch(p,r,l)  (void)(p)
#endif

/* ══════════════════════════════════════════════════════════════════
 * Linux / macOS — standard headers
 * ══════════════════════════════════════════════════════════════════ */
#else /* Linux / macOS */

#define _POSIX_C_SOURCE 200809L
#define _GNU_SOURCE

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/types.h>

/* pogls_fsync = standard fsync */
#define pogls_fsync(fd)  fsync(fd)

#endif /* platform */

/* ══════════════════════════════════════════════════════════════════
 * Common across all platforms
 * ══════════════════════════════════════════════════════════════════ */

/* compiler hints */
#ifndef likely
  #define likely(x)    __builtin_expect(!!(x), 1)
  #define unlikely(x)  __builtin_expect(!!(x), 0)
#endif

/* cache line size */
#ifndef POGLS_CACHE_LINE
  #define POGLS_CACHE_LINE  64u
#endif

/* align helper */
#ifdef POGLS_WINDOWS
  #define POGLS_ALIGN(n)  __declspec(align(n))
#else
  #define POGLS_ALIGN(n)  __attribute__((aligned(n)))
#endif

/* packed struct */
#ifdef POGLS_WINDOWS
  #define POGLS_PACKED  __pragma(pack(push,1)) 
  #define POGLS_PACKED_END __pragma(pack(pop))
#else
  #define POGLS_PACKED      /* use __attribute__((packed)) per struct */
  #define POGLS_PACKED_END
#endif

/* path separator */
#ifdef POGLS_WINDOWS
  #define POGLS_PATH_SEP  "\\"
#else
  #define POGLS_PATH_SEP  "/"
#endif

/* snprintf path helper */
#define POGLS_PATH(buf, dir, file) \
    snprintf(buf, sizeof(buf), "%s" POGLS_PATH_SEP "%s", dir, file)

/* version */
#define POGLS_VERSION_MAJOR  3
#define POGLS_VERSION_MINOR  9
#define POGLS_VERSION_PATCH  1
#define POGLS_VERSION_STR    "3.9.1"

#endif /* POGLS_PLATFORM_H */

/* ══════════════════════════════════════════════════════════════════
 * PHI ADDRESSING CONSTANTS (SINGLE SOURCE — FROZEN)
 * All subsystems MUST reference these via pogls_platform.h
 * Never redefine elsewhere.
 * ══════════════════════════════════════════════════════════════════ */
#ifndef POGLS_PHI_CONSTANTS
#define POGLS_PHI_CONSTANTS
#  define POGLS_PHI_SCALE   (1u  << 20)    /* 2^20 = 1,048,576         */
#  define POGLS_PHI_UP      1696631u        /* floor(phi  x 2^20)       */
#  define POGLS_PHI_DOWN     648055u        /* floor(phi^-1 x 2^20)     */
#  define POGLS_PHI_COMP     400521u        /* 2^20 - PHI_DOWN (wrap)   */
#endif
