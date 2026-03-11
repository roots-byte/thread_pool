/* 
 * Copyright (c) 2026 Jonáš Rys
 * Licensed under the MIT License. See LICENSE file for details.
 */
/**
 * @file thread_pool.h
 * @brief Lightweight C thread pool with task queue and worker lifecycle management.
 *
 * Supervisor thread dispatches tasks from a dynamically growing queue to N worker threads.
 * Include thread_pool.c directly in one translation unit (all functions are static inline).
 *
 * Quick start:
 *   1. Extend task_t and pool_worker_t with your own fields (>>> markers).
 *   2. Implement worker_func, set job_status = TASK_STATUS_FINISHED before returning 0.
 *   3. CreatePool() -> AddTaskToPool() -> WaitForFinishPool() -> DestroyPool().
 */

#ifndef THREAD_POOL_H
#define THREAD_POOL_H

#ifdef __cplusplus
extern "C" {
#endif

#include "thread_pool_wrapper.h"
#include <stdint.h>


/* Timeout and tuning constants (ms). Define before including to override. */

#ifndef WAIT_FOR_VALUE_SLEEP_MS
#define WAIT_FOR_VALUE_SLEEP_MS 1
#endif

#ifndef MAX_RUNNING_TASK_TIME_MS
#define MAX_RUNNING_TASK_TIME_MS 10000
#endif

#ifndef MAX_TIME_IN_QUEUE_TIME_MS
#define MAX_TIME_IN_QUEUE_TIME_MS 10000
#endif

#ifndef THREAD_SLEEP_TIME_MS
#define THREAD_SLEEP_TIME_MS 1
#endif

#ifndef POOL_LOOP_TIMEOUT_MS
#define POOL_LOOP_TIMEOUT_MS 1
#endif

#ifndef MAX_POOL_SHUTDOWN_TIME_MS
#define MAX_POOL_SHUTDOWN_TIME_MS 2000
#endif


/* Enumerations */

/* Task lifecycle state. worker_func must set job_status = TASK_STATUS_FINISHED before returning 0. */
enum TASK_STATUS
{
    TASK_STATUS_CREATED = 0, /**< Task allocated, not yet in queue.           */
    TASK_STATUS_IN_QUEUE,    /**< Task accepted by AddTaskToPool().            */
    TASK_STATUS_RUNNING,     /**< Worker thread is executing the task.         */
    TASK_STATUS_FINISHED,    /**< worker_func completed successfully.          */
    TASK_STATUS_ERROR,       /**< worker_func returned non-zero.               */
    TASK_STATUS_STOP         /**< Task is being destroyed (internal).          */
};

/* Internal: supervisor <-> worker thread state machine. */
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

/* Internal: pool supervisor thread state machine. */
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


/* Forward declarations */

typedef struct pool_t pool_t;
typedef struct pool_worker_t pool_worker_t;
typedef struct init_pool_worker_t init_pool_worker_t;
typedef struct task_queue_t task_queue_t;
typedef struct task_t task_t;

/* One unit of work. Allocate with CreateTask(), fill fields, submit with AddTaskToPool(). */
typedef struct task_t
{
    atomic_int_t job_status;  /* current lifecycle state (TASK_STATUS_*) */

    /* >>> Add your own task input/output fields here <<< */

} task_t;

/* One worker thread in the pool. Access worker->task inside worker_func. */
typedef struct pool_worker_t
{
    thread_t        thread;
    pool_t*         pool;
    uint32_t        start_time;
    task_t*         task;          /* current task — read this inside worker_func */
    atomic_int_t    thread_status;
    atomic_int_t    pool_status;

    /* >>> Add your own per-thread state fields here <<< */

} pool_worker_t;

/* Internal pool state. Treat as opaque — use the public API only. */
typedef struct pool_t
{
    atomic_int_t    status;
    atomic_int_t    communication_status;
    task_queue_t*   queue;
    atomic_ptr_t*   running_tasks;
    thread_t*       thread;
    pool_worker_t** threads;
    int             num_threads;
    init_pool_worker_t* init_data;
} pool_t;

/*
 * Pool configuration callbacks. Pass to CreatePool().
 * worker_func is required and must set job_status = TASK_STATUS_FINISHED before returning 0.
 */
typedef struct init_pool_worker_t
{
    int  (*init_func)(init_pool_worker_t*, pool_worker_t*);    /* optional: per-worker init */
    int  (*worker_func)(pool_worker_t*);                       /* required: task handler    */
    void (*destroy_func)(pool_worker_t*);                      /* optional: per-worker cleanup */

    /* >>> Add your own shared pool-wide data fields here <<< */

} init_pool_worker_t;


/* Task API */

static inline task_t* CreateTask(void);
static inline void DestroyTask(task_t* task);
static inline int WaitForFinishTask(task_t* task);

/* Pool API */

static inline pool_t* CreatePool(int thread_count, init_pool_worker_t* init);
static inline void DestroyPool(pool_t* pool);
static inline int AddTaskToPool(pool_t* pool, task_t* task);
static inline int WaitForFinishPool(pool_t* pool);
static inline int ClearTasksInPool(pool_t* pool);

#ifndef QUEUE_CAPACITY
#define QUEUE_CAPACITY 1024
#endif

typedef struct task_queue_t
{
    atomic_ptr_t* tasks;
    atomic_int_t capacity;
    atomic_int_t head;
    atomic_int_t tail;
    atomic_int_t running; 
    mutex_t mutex; // protects resizing the queue
    cond_t cond;   // signals when new tasks are added to the queue
} task_queue_t;

static inline task_queue_t* CreateTaskQueue(void);
static inline void DestroyTaskQueue(task_queue_t* queue);
static inline int EnqueueTask(task_queue_t* queue, task_t* task);
static inline task_t* DequeueTask(task_queue_t* queue);

#ifdef __cplusplus
}
#endif
#endif // THREAD_POOL_H
