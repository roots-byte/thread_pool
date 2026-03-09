/* 
 * Copyright (c) 2026 Jonáš Rys
 * Licensed under the MIT License. See LICENSE file for details.
 */
/**
 * @file thread_pool.h
 * @brief Lightweight, extensible C thread pool with task queue and worker lifecycle management.
 *
 * @details
 * This header-only library implements a thread pool backed by a dedicated supervisor thread
 * (PoolWorker) and N worker threads (PoolThreadWorker). Tasks are submitted from any thread,
 * placed into a dynamically growing queue, and dispatched to idle workers automatically.
 *
 * ### Architecture
 * ```
 *  caller thread(s)
 *       │
 *       │  AddTaskToPool()
 *       ▼
 *  [ task_to_add ] ──► PoolWorker (supervisor thread)
 *                              │
 *                    places into queue_tasks[]
 *                              │
 *              dispatches to idle pool_worker_t threads
 *                              │
 *                    worker calls worker_func(worker)
 * ```
 *
 * ### Quick start
 * 1. Extend `task_t` and `pool_worker_t` with your own fields.
 * 2. Implement `worker_func` — it reads `worker->task`, does the work, and sets
 *    `job_status = TASK_STATUS_FINISHED`.
 * 3. Fill an `init_pool_worker_t` and call `CreatePool()`.
 * 4. For each unit of work: `CreateTask()` → fill fields → `AddTaskToPool()`.
 * 5. `WaitForFinishPool()` → `DestroyPool()`.
 *
 * ### Thread safety
 * All public functions are safe to call from any thread **except** the worker function itself.
 * `DestroyPool()` must not be called while tasks are still being submitted.
 *
 * ### Customization
 * Search for `>>>` marker comments in the structs — those are the only places
 * you should add fields. Do **not** modify the internal bookkeeping fields.
 *
 * @note All implementation functions are `static inline` and live in `thread_pool.c`.
 *       Include `thread_pool.c` directly in one translation unit (not as a compiled object).
 */

#ifndef THREAD_POOL_H
#define THREAD_POOL_H

#ifdef __cplusplus
extern "C" {
#endif

#include "thread_pool_wrapper.h"
#include <stdint.h>


/* =========================================================================
 * Timeout and tuning constants
 * =========================================================================
 * All values are in milliseconds.  Define any of these *before* including
 * this header to override the defaults.
 * ========================================================================= */

/** @brief Sleep interval inside WaitForValue() polling loops. Lower = faster response, higher CPU. */
#ifndef WAIT_FOR_VALUE_SLEEP_MS
#define WAIT_FOR_VALUE_SLEEP_MS 2
#endif

/** @brief Maximum time a single task is allowed to run before the pool considers the worker hung
 *         and attempts to recreate it. Increase for long-running tasks. */
#ifndef MAX_RUNNING_TASK_TIME_MS
#define MAX_RUNNING_TASK_TIME_MS 10000
#endif

/** @brief Maximum time a task may spend waiting in the queue before WaitForFinishTask() times out. */
#ifndef MAX_TIME_IN_QUEUE_TIME_MS
#define MAX_TIME_IN_QUEUE_TIME_MS 10000
#endif

/** @brief Sleep interval of the supervisor (PoolWorker) and worker (PoolThreadWorker) idle loops. */
#ifndef THREAD_SLEEP_TIME_MS
#define THREAD_SLEEP_TIME_MS 2
#endif

/** @brief Idle sleep of the supervisor (PoolWorker) main dispatch loop.
 *         Lower = faster task pickup latency, slightly higher CPU use when idle. */
#ifndef POOL_LOOP_TIMEOUT_MS
#define POOL_LOOP_TIMEOUT_MS 1
#endif

/** @brief Maximum time DestroyPool() waits for the supervisor thread to stop gracefully
 *         before force-killing it. */
#ifndef MAX_POOL_SHUTDOWN_TIME_MS
#define MAX_POOL_SHUTDOWN_TIME_MS 2000
#endif

/** @brief Sleep between retries inside AddTaskToPool() when the queue handoff slot is busy. */
#ifndef SLEEP_FOR_ADD_TASK_TIME_MS
#define SLEEP_FOR_ADD_TASK_TIME_MS 2
#endif

/** @brief Total time AddTaskToPool() will spin waiting for a free handoff slot before giving up. */
#ifndef MAX_TIME_FOR_ADD_TASK
#define MAX_TIME_FOR_ADD_TASK 10000
#endif

/** @brief Initial capacity of the task queue (number of task pointers).
 *         The queue doubles automatically when full. */
#ifndef INITIAL_QUEUE_SIZE
#define INITIAL_QUEUE_SIZE 100
#endif

/* =========================================================================
 * Enumerations  (internal — do not set these manually from outside the pool)
 * ========================================================================= */

/**
 * @brief Lifecycle state of a single task.
 *
 * Written atomically by the pool/worker; read by the caller via WaitForFinishTask().
 *
 * @note The worker_func **must** set `job_status = TASK_STATUS_FINISHED` before returning 0.
 *       Failing to do so will cause WaitForFinishTask() to block until it times out.
 */
enum TASK_STATUS
{
    TASK_STATUS_CREATED = 0, /**< Task allocated, not yet in queue.           */
    TASK_STATUS_IN_QUEUE,    /**< Task accepted by AddTaskToPool().            */
    TASK_STATUS_RUNNING,     /**< Worker thread is executing the task.         */
    TASK_STATUS_FINISHED,    /**< worker_func completed successfully.          */
    TASK_STATUS_ERROR,       /**< worker_func returned non-zero.               */
    TASK_STATUS_STOP         /**< Task is being destroyed (internal).          */
};

/**
 * @brief State machine values shared between the supervisor and a worker thread.
 *
 * @note Internal — not intended for use outside of thread_pool.c.
 */
enum THREAD_WORKER_STATUS
{
    THREAD_WORKER_STATUS_IDLE,    /**< Worker waiting for a task.              */
    THREAD_WORKER_STATUS_INIT,    /**< Worker thread starting up.              */
    THREAD_WORKER_STATUS_WORKING, /**< Worker executing a task.                */
    THREAD_WORKER_STATUS_ERROR,   /**< Worker encountered a fatal error.       */
    THREAD_WORKER_STATUS_STOP,    /**< Worker thread has exited.               */
    THREAD_WORKER_SET_OK,         /**< Supervisor → worker: do nothing.        */
    THREAD_WORKER_SET_STOP,       /**< Supervisor → worker: exit loop.         */
    THREAD_WORKER_SET_DO_TASK     /**< Supervisor → worker: execute task.      */
};

/**
 * @brief State machine values for the pool supervisor thread.
 *
 * @note Internal — not intended for use outside of thread_pool.c.
 */
enum POOL_STATUS
{
    POOL_STATUS_INIT,        /**< Pool is starting up.                         */
    POOL_STATUS_STOP,        /**< Supervisor thread has exited.                */
    POOL_STATUS_ERROR,       /**< Fatal error inside supervisor.               */
    POOL_STATUS_EMPTY,       /**< Queue is empty and all workers are idle.     */
    POOL_STATUS_OK,          /**< Queue has pending tasks.                     */
    POOL_SET_OK,             /**< Main → supervisor: normal operation.         */
    POOL_SET_STOP,           /**< Main → supervisor: shut down.                */
    POOL_SET_CLEAR_TASKS     /**< Main → supervisor: discard all pending tasks.*/
};


/* =========================================================================
 * Forward declarations
 * ========================================================================= */

typedef struct pool_t pool_t;
typedef struct pool_worker_t pool_worker_t;
typedef struct init_pool_worker_t init_pool_worker_t;

/**
 * @brief Represents one unit of work submitted to the pool.
 *
 * @details Allocate with CreateTask(), fill your custom fields, submit with AddTaskToPool().
 * After the task finishes, read your results, then free with DestroyTask().
 *
 * @warning Do **not** modify `job_status` or `task_status` directly from user code.
 *          Use WaitForFinishTask() to wait for completion and check the result.
 */
typedef struct
{
    atomic_int_t job_status;  /**< @internal Pool→main: current lifecycle state (TASK_STATUS_*). */
    atomic_int_t task_status; /**< @internal Main→pool: reserved for future use.                 */
    uint32_t add_time;        /**< @internal Timestamp set when the task enters the queue.        */

    /* ------------------------------------------------------------------ */
    /* >>>  Add your own task input/output fields here  <<< */
    /* ------------------------------------------------------------------ */
    int      task_id;
    uint64_t num_samples;   /**< input:  how many random points to generate */
    uint64_t hits;          /**< output: how many fell inside the unit circle */

} task_t;

/**
 * @brief Represents one worker thread managed by the pool.
 *
 * @details One instance exists per thread in the pool. The supervisor passes a pointer to this
 * struct as the argument to `worker_func`. Access `worker->task` to get the current task.
 *
 * @warning Do **not** modify any of the bookkeeping fields. Only `task` (read-only) and your
 *          own added fields are meant to be accessed inside `worker_func`.
 */
typedef struct pool_worker_t
{
    thread_t        thread;        /**< @internal OS thread handle.                              */
    pool_t*         pool;          /**< @internal Back-pointer to the owning pool.               */
    uint32_t        start_time;    /**< @internal Timestamp when current task execution started. */
    task_t*         task;          /**< Current task — read this inside worker_func.             */
    atomic_int_t    thread_status; /**< @internal Worker→supervisor: current state.              */
    atomic_int_t    pool_status;   /**< @internal Supervisor→worker: command channel.            */

    /* ------------------------------------------------------------------ */
    /* >>>  Add your own per-thread state fields here  <<< */
    /* ------------------------------------------------------------------ */
    unsigned int rand_seed;        /**< per-worker RNG seed (set in init_func) */

} pool_worker_t;

/**
 * @brief Internal state of the thread pool.
 *
 * @note Treat as opaque. Never read or write any fields directly from user code.
 *       Use the public API functions (CreatePool, AddTaskToPool, etc.) exclusively.
 */
typedef struct pool_t
{
    atomic_int_t    status;               /**< @internal Pool→main: current pool state.          */
    atomic_int_t    communication_status; /**< @internal Main→pool: command channel.             */
    task_t**        queue_tasks;          /**< @internal Dynamically sized array of queued tasks.*/
    int             queue_size;           /**< @internal Current allocated capacity of the queue.*/
    task_t**        running_tasks;        /**< @internal Tasks currently running (one per thread)*/
    thread_t*       thread;               /**< @internal Supervisor thread handle.               */
    pool_worker_t** threads;              /**< @internal Array of worker thread structs.         */
    int             num_threads;          /**< @internal Number of worker threads.               */
    atomic_int_t    free_to_add_task;     /**< @internal Handoff slot state (1=free, 0=ready, 2=writing). */
    task_t*         task_to_add;          /**< @internal Single-slot handoff buffer for AddTaskToPool.    */
    init_pool_worker_t* init_data;        /**< @internal Callbacks and user data passed to CreatePool.    */
} pool_t;

/**
 * @brief Configuration and callbacks provided by the user when creating a pool.
 *
 * @details Fill this struct and pass it to CreatePool(). The pool holds a pointer to it for
 * its entire lifetime — **do not free or modify it while the pool is running**.
 *
 * ### Callbacks
 * | Callback       | Called by          | Called when                                  | Return |
 * |----------------|--------------------|----------------------------------------------|--------|
 * | `init_func`    | CreateThreadWorker | Each worker thread is created (incl. restart)| 0 = ok, non-zero = abort |
 * | `worker_func`  | PoolThreadWorker   | A task is dispatched to the worker           | 0 = ok, non-zero = error |
 * | `destroy_func` | DestroyThreadWorker| A worker thread is being torn down           | void   |
 *
 * @warning `worker_func` **must** set `worker->task->job_status = TASK_STATUS_FINISHED`
 *          before returning 0.
 * @warning `worker_func` must never be NULL — CreatePool() will fail if it is.
 */
typedef struct init_pool_worker_t
{
    /**
     * @brief Optional. Called once per worker thread on creation (and on recreation after a crash).
     * @param[in]  init_data  Pointer to this struct — use to pass shared config to the worker.
     * @param[out] worker     The newly created worker — set per-thread fields here.
     * @return 0 on success, non-zero to abort thread creation.
     */
    int  (*init_func)(init_pool_worker_t*, pool_worker_t*);

    /**
     * @brief Required. Called by the worker thread for each dispatched task.
     * @param[in,out] worker  The calling worker thread; access the task via worker->task.
     * @return 0 on success. Non-zero marks the task as TASK_STATUS_ERROR and kills the worker.
     * @note Must set `worker->task->job_status = TASK_STATUS_FINISHED` before returning 0.
     */
    int  (*worker_func)(pool_worker_t*);

    /**
     * @brief Optional. Called when a worker thread is being destroyed (graceful or forced).
     * @param[in] worker  The worker being torn down — free any per-thread resources here.
     */
    void (*destroy_func)(pool_worker_t*);

    /* ------------------------------------------------------------------ */
    /* >>>  Add your own shared pool-wide data fields here  <<< */
    /* ------------------------------------------------------------------ */

} init_pool_worker_t;


/* =========================================================================
 * Task API
 * ========================================================================= */

/**
 * @brief Allocates and initialises a new task. Sets `job_status = TASK_STATUS_CREATED`.
 * @return Pointer to the new task, or NULL on allocation failure.
 * @note The task must be freed with DestroyTask() after use.
 */
static inline task_t* CreateTask(void);

/**
 * @brief Waits for any in-progress execution to finish, then frees the task.
 * @param[in] task  Task to destroy. Silently ignored if NULL.
 * @warning After this call the pointer is invalid.
 * @warning Do **not** call from inside `worker_func`.
 */
static inline void DestroyTask(task_t* task);

/**
 * @brief Blocks until the task completes each lifecycle stage, or until timeout.
 * @details Waits through: CREATED → IN_QUEUE → RUNNING → done, with per-stage timeouts.
 * @param[in] task  Task to wait for.
 * @return 0 on success (task finished), -1 on timeout or invalid input.
 * @note `worker_func` **must** set `job_status = TASK_STATUS_FINISHED` before returning 0.
 */
static inline int WaitForFinishTask(task_t* task);


/* =========================================================================
 * Pool API
 * ========================================================================= */

/**
 * @brief Creates a pool with `thread_count` worker threads and one supervisor thread.
 * @details Waits for all workers to reach IDLE before returning.
 * @param[in] thread_count  Number of parallel worker threads. Must be ≥ 1.
 * @param[in] init          Pointer to a filled init_pool_worker_t. Must stay valid for pool lifetime.
 * @return Pointer to the new pool, or NULL on failure.
 */
static inline pool_t* CreatePool(int thread_count, init_pool_worker_t* init);

/**
 * @brief Signals shutdown, joins all threads, frees all memory.
 * @param[in] pool  Pool to destroy. Silently ignored if NULL.
 * @warning Call WaitForFinishPool() first if you need all tasks to complete before shutdown.
 */
static inline void DestroyPool(pool_t* pool);

/**
 * @brief Submits a task to the pool via atomic CAS handoff.
 * @return 0 on success, -1 on timeout or if pool is stopping.
 * @note The pool takes ownership — do not free task until after WaitForFinishTask().
 */
static inline int AddTaskToPool(pool_t* pool, task_t* task);

/**
 * @brief Blocks until the queue is empty and all workers are idle.
 * @return 0 on success, -1 on timeout or invalid/stopping pool.
 */
static inline int WaitForFinishPool(pool_t* pool);

/**
 * @brief Discards all queued tasks and waits for running ones to finish.
 * @return 0 on success, -1 if pool is stopping or clearing times out.
 */
static inline int ClearTasksInPool(pool_t* pool);


#ifdef __cplusplus
}
#endif
#endif // THREAD_POOL_H
