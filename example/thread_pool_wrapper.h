/* 
 * Copyright (c) 2026 Jonáš Rys
 * Licensed under the MIT License. See LICENSE file for details.
 */

#ifndef THREAD_POOL_WRAPPER_H
#define THREAD_POOL_WRAPPER_H

#ifndef _WIN32
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 199309L
#endif
#endif

#ifdef _WIN32
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif
#include <windows.h>
#include <stdint.h>
#else
#include <stdint.h>
#endif


// ===================== Thread =====================
typedef void* (*thread_func_t)(void*);

#ifdef _WIN32
  #include <windows.h>
  #include <stdlib.h>
  typedef HANDLE thread_t;

  typedef struct {
      thread_func_t func;
      void* arg;
  } thread_start_t;

  static inline DWORD WINAPI thread_start(LPVOID arg)
  {
      thread_start_t* start = (thread_start_t*)arg;
      start->func(start->arg);
      free(start);
      return 0;
  }

  static inline int thread_create(thread_t* thread, thread_func_t func, void* arg)
  {
      thread_start_t* start = (thread_start_t*)malloc(sizeof(thread_start_t));
      if (!start) return -1;
      start->func = func;
      start->arg = arg;

      *thread = CreateThread(NULL, 0, thread_start, start, 0, NULL);
      return *thread ? 0 : -1;
  }

  static inline void thread_join(thread_t thread)
  {
      WaitForSingleObject(thread, INFINITE);
      CloseHandle(thread);
  }

  static inline void thread_destroy(thread_t thread)
  {
      if (WaitForSingleObject(thread, 5000) != WAIT_OBJECT_0)
          TerminateThread(thread, 1);
      CloseHandle(thread);
  }

#else
  #include <pthread.h>
  typedef pthread_t thread_t;

  static inline int thread_create(thread_t* thread, thread_func_t func, void* arg)
  {
      return pthread_create(thread, NULL, func, arg);
  }

  static inline void thread_join(thread_t thread)
  {
      pthread_join(thread, NULL);
  }

  static inline void thread_destroy(thread_t thread)
  {
      pthread_cancel(thread);
      pthread_join(thread, NULL);
  }
#endif


// ===================== Atomic =====================
#ifndef _WIN32
#include <stdint.h>
#endif

typedef struct {
#ifdef _WIN32
    volatile LONG value;
#else
    volatile int value;
#endif
} atomic_int_t;

typedef struct {
    void* volatile ptr;
} atomic_ptr_t;

static inline void atomic_int_set(atomic_int_t* a, int v) {
#ifdef _WIN32
    InterlockedExchange(&a->value, v);
#else
    __sync_lock_test_and_set(&a->value, v);
#endif
}

static inline int atomic_int_get(atomic_int_t* a) {
#ifdef _WIN32
    return InterlockedCompareExchange(&a->value, 0, 0);
#else
    return __sync_val_compare_and_swap(&a->value, 0, 0);
#endif
}

static inline int atomic_int_inc(atomic_int_t* a) {
#ifdef _WIN32
    return InterlockedIncrement(&a->value);
#else
    return __sync_add_and_fetch(&a->value, 1);
#endif
}

static inline int atomic_int_dec(atomic_int_t* a) {
#ifdef _WIN32
    return InterlockedDecrement(&a->value);
#else
    return __sync_sub_and_fetch(&a->value, 1);
#endif
}

static inline void* atomic_ptr_set(atomic_ptr_t* a, void* p) {
#ifdef _WIN32
    return (void*)InterlockedExchangePointer((PVOID*)&a->ptr, p);
#else
    return (void*)__sync_lock_test_and_set(&a->ptr, p);
#endif
}

static inline void* atomic_ptr_get(atomic_ptr_t* a) {
#ifdef _WIN32
    return InterlockedCompareExchangePointer((PVOID*)&a->ptr, NULL, NULL);
#else
    return __sync_val_compare_and_swap(&a->ptr, NULL, NULL);
#endif
}

static inline int atomic_int_cas(atomic_int_t* a, int expected, int desired) {
#ifdef _WIN32
    LONG old = InterlockedCompareExchange((LONG*)&a->value, desired, expected);
    return old == expected;
#else
    int old = __sync_val_compare_and_swap(&a->value, expected, desired);
    return old == expected;
#endif
}

static inline void* atomic_ptr_cas(atomic_ptr_t* a, void* expected, void* desired) {
#ifdef _WIN32
    void* current = InterlockedCompareExchangePointer((PVOID*)&a->ptr, desired, expected);
#else
    void* current = __sync_val_compare_and_swap(&a->ptr, expected, desired);
#endif
    return current;
}

// ===================== Mutex =====================
#ifdef _WIN32
typedef CRITICAL_SECTION mutex_t;
#else
typedef pthread_mutex_t mutex_t;
#endif

static inline int mutex_init(mutex_t* m) {
#ifdef _WIN32
    InitializeCriticalSection(m);
    return 0;
#else
    return pthread_mutex_init(m, NULL);
#endif
}

static inline int mutex_lock(mutex_t* m) {
#ifdef _WIN32
    EnterCriticalSection(m);
    return 0;
#else
    return pthread_mutex_lock(m);
#endif
}

static inline int mutex_trylock(mutex_t* m) {
#ifdef _WIN32
    return TryEnterCriticalSection(m) ? 0 : -1;
#else
    return pthread_mutex_trylock(m);
#endif
}

static inline int mutex_unlock(mutex_t* m) {
#ifdef _WIN32
    LeaveCriticalSection(m);
    return 0;
#else
    return pthread_mutex_unlock(m);
#endif
}

static inline void mutex_destroy(mutex_t* m) {
#ifdef _WIN32
    DeleteCriticalSection(m);
#else
    pthread_mutex_destroy(m);
#endif
}


// ===================== Condition Variable =====================
#ifdef _WIN32
typedef CONDITION_VARIABLE cond_t;
#else
typedef pthread_cond_t cond_t;
#endif

static inline int cond_init(cond_t* c) {
#ifdef _WIN32
    InitializeConditionVariable(c);
    return 0;
#else
    return pthread_cond_init(c, NULL);
#endif
}

static inline int cond_wait(cond_t* c, mutex_t* m) {
#ifdef _WIN32
    return SleepConditionVariableCS(c, m, INFINITE) ? 0 : -1;
#else
    return pthread_cond_wait(c, m);
#endif
}

static inline int cond_timedwait(cond_t* c, mutex_t* m, int timeout_ms) {
#ifdef _WIN32
    return SleepConditionVariableCS(c, m, (DWORD)timeout_ms) ? 0 : -1;
#else
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec  += timeout_ms / 1000;
    ts.tv_nsec += (timeout_ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) {
        ts.tv_sec  += 1;
        ts.tv_nsec -= 1000000000L;
    }
    return pthread_cond_timedwait(c, m, &ts);
#endif
}

static inline int cond_signal(cond_t* c) {
#ifdef _WIN32
    WakeConditionVariable(c);
    return 0;
#else
    return pthread_cond_signal(c);
#endif
}

static inline int cond_broadcast(cond_t* c) {
#ifdef _WIN32
    WakeAllConditionVariable(c);
    return 0;
#else
    return pthread_cond_broadcast(c);
#endif
}

static inline void cond_destroy(cond_t* c) {
#ifdef _WIN32
    (void)c; // Windows CONDITION_VARIABLE doesn't need destruction
#else
    pthread_cond_destroy(c);
#endif
}


// ===================== time wrapper =====================
#ifdef _WIN32
    #include <Windows.h>
#else
    #ifndef CLOCK_MONOTONIC
    #define CLOCK_MONOTONIC CLOCK_REALTIME
    #endif
    #include <time.h>
    #include <stdint.h>
    #include <unistd.h>
#endif

static inline void sleep_ns(int64_t ns) {
#ifdef _WIN32
    if (ns >= 1000000) {
        Sleep((DWORD)(ns / 1000000));
        ns %= 1000000;
    }
    if (ns > 0) {
        LARGE_INTEGER freq, start, now;
        QueryPerformanceFrequency(&freq);
        QueryPerformanceCounter(&start);
        INT64 target = start.QuadPart + (freq.QuadPart * ns) / 1000000000;
        do { QueryPerformanceCounter(&now); } while (now.QuadPart < target);
    }
#else
    struct timespec ts;
    ts.tv_sec  = (time_t)(ns / 1000000000);
    ts.tv_nsec = (long)(ns % 1000000000);
    nanosleep(&ts, NULL);
#endif
}

static inline void sleep_ms(int ms) {
#ifdef _WIN32
    Sleep(ms);
#else
    struct timespec ts;
    ts.tv_sec  = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000;
    nanosleep(&ts, NULL);
#endif
}

static inline void sleep_us(int us) {
#ifdef _WIN32
    // Windows Sleep has ms granularity; busy-wait for sub-ms
    if (us >= 1000) {
        Sleep(us / 1000);
        us %= 1000;
    }
    if (us > 0) {
        LARGE_INTEGER freq, start, now;
        QueryPerformanceFrequency(&freq);
        QueryPerformanceCounter(&start);
        INT64 target = start.QuadPart + (freq.QuadPart * us) / 1000000;
        do { QueryPerformanceCounter(&now); } while (now.QuadPart < target);
    }
#else
    struct timespec ts;
    ts.tv_sec  = us / 1000000;
    ts.tv_nsec = (us % 1000000) * 1000;
    nanosleep(&ts, NULL);
#endif
}

static inline uint32_t get_time_ms(void) {
#ifdef _WIN32
    return (uint32_t)GetTickCount();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
#endif
}

#endif // THREAD_POOL_WRAPPER_H
