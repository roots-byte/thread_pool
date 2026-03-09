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
      free(start); // uvolníme mallocovaný start
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
      WaitForSingleObject(thread, INFINITE); // počká, pokud běží
      CloseHandle(thread);                    // zavře handle
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
    // cancel a join
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
    volatile LONG value;        // pro int
#else
    volatile int value;
#endif
} atomic_int_t;

typedef struct {
#ifdef _WIN32
    volatile LONG_PTR ptr;      // pro pointer
#else
    void* volatile ptr;
#endif
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

static inline void atomic_ptr_set(atomic_ptr_t* a, void* p) {
#ifdef _WIN32
    InterlockedExchangePointer((PVOID*)&a->ptr, p);
#else
    (void)__sync_lock_test_and_set(&a->ptr, p);
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

static inline void sleep_ms(int ms) {
#ifdef _WIN32
    Sleep(ms); // Sleep expects milliseconds
#else
    struct timespec ts;
    ts.tv_sec  = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000; // convert ms to ns
    nanosleep(&ts, NULL);
#endif
}

static inline uint32_t  get_time_ms(void) {
#ifdef _WIN32
    return (uint32_t)GetTickCount();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
#endif
}

#endif // THREAD_POOL_WRAPPER_H