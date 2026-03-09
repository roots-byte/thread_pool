/* 
 * Copyright (c) 2026 Jonáš Rys
 * Licensed under the MIT License. See LICENSE file for details.
 */

#include "thread_pool.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* internal forward declarations */
static inline void* PoolThreadWorker(void* arg);
static inline void* PoolWorker(void* arg);


static inline int WaitForValue(void* variable, int value, char atomic, int timeout_ms, char match)
{
    int elapsed = 0;
    uint32_t start_time = get_time_ms();

    while (elapsed < timeout_ms)
    {
        int current_value;

        if (variable == NULL)
        {
            fprintf(stderr, "[thread_pool] WaitForValue: variable is NULL\n");
            return -2; // invalid variable
        }

        if (atomic)
            current_value = atomic_int_get((atomic_int_t*)variable);
        else
            current_value = *(int*)variable;

        if (match && current_value == value)
            return 0; // value matched
        else if (!match && current_value != value)
            return 0; // value not matched

        sleep_ms(WAIT_FOR_VALUE_SLEEP_MS);
        elapsed = get_time_ms() - start_time;
        if (elapsed < 0)
        {
            start_time = get_time_ms();
            elapsed = 0;
        }
    }

    return -1; // timeout
}

//Task functions

static inline task_t* CreateTask(void)
{
    task_t* task = (task_t*)malloc(sizeof(task_t));
    if (!task) { fprintf(stderr, "[thread_pool] CreateTask: malloc failed\n"); return NULL; }

    atomic_int_set(&task->job_status, TASK_STATUS_CREATED); // inicializace job_status
    return task;
}

static inline void DestroyTask(task_t* task)
{
    if (task == NULL) return;

    WaitForValue(&task->job_status, TASK_STATUS_RUNNING, 1, MAX_RUNNING_TASK_TIME_MS, 0);

    atomic_int_set(&task->job_status, TASK_STATUS_STOP);
    sleep_ms(WAIT_FOR_VALUE_SLEEP_MS * 2);
    free(task);

}

// Wait until the worker function has signalled TASK_STATUS_FINISHED.
// The user's worker_func MUST set job_status = TASK_STATUS_FINISHED on success.
static inline int WaitForFinishTask(task_t* task)
{
    if (task == NULL) { fprintf(stderr, "[thread_pool] WaitForFinishTask: task is NULL\n"); return -1; }

    if (WaitForValue(&task->job_status, TASK_STATUS_CREATED, 1, MAX_TIME_IN_QUEUE_TIME_MS, 0) != 0)
    {
        fprintf(stderr, "[thread_pool] WaitForFinishTask: timeout waiting for task to start\n");
        return -1;
    }

    if (WaitForValue(&task->job_status, TASK_STATUS_IN_QUEUE, 1, MAX_TIME_IN_QUEUE_TIME_MS + MAX_RUNNING_TASK_TIME_MS, 0) != 0)
    {
        fprintf(stderr, "[thread_pool] WaitForFinishTask: timeout waiting for task to leave queue\n");
        return -1;
    }

    if (WaitForValue(&task->job_status, TASK_STATUS_RUNNING, 1, MAX_RUNNING_TASK_TIME_MS, 0) != 0)
    {
        fprintf(stderr, "[thread_pool] WaitForFinishTask: timeout waiting for task to finish\n");
        return -1;
    }

    return 0;
}

//Pool worker functions

static inline pool_worker_t* CreateThreadWorker(pool_t* pool)
{
    pool_worker_t* worker = (pool_worker_t*)malloc(sizeof(pool_worker_t));
    if (!worker) { fprintf(stderr, "[thread_pool] CreateThreadWorker: malloc failed\n"); return NULL; }

    atomic_int_set(&worker->thread_status, THREAD_WORKER_STATUS_INIT);
    atomic_int_set(&worker->pool_status, THREAD_WORKER_SET_OK);
    
    worker->task = NULL;
    worker->start_time = 0;
    worker->pool = pool;

    init_pool_worker_t* init_data = worker->pool->init_data;

    if (init_data->worker_func == NULL)
    {
        fprintf(stderr, "[thread_pool] CreateThreadWorker: worker_func is NULL\n");
        free(worker);
        return NULL; // no worker function defined
    }

    if (init_data->init_func)
    {
        if (init_data->init_func(init_data, worker))
        {
            fprintf(stderr, "[thread_pool] CreateThreadWorker: init_func failed\n");
            free(worker);
            return NULL; // error during init
        }
    }

    if (thread_create(&worker->thread, PoolThreadWorker, (void*)worker) != 0)
    {
        fprintf(stderr, "[thread_pool] CreateThreadWorker: thread_create failed\n");
        free(worker);
        return NULL; // error creating thread
    }

    if (WaitForValue(&worker->thread_status, THREAD_WORKER_STATUS_IDLE, 1, MAX_RUNNING_TASK_TIME_MS, 1) != 0)
    {
        fprintf(stderr, "[thread_pool] CreateThreadWorker: worker thread did not become idle in time\n");
        thread_destroy(worker->thread);
        free(worker);
        return NULL;
    }

    return worker;

}

static inline void DestroyThreadWorker(pool_worker_t* worker)
{
    atomic_int_set(&worker->pool_status, THREAD_WORKER_SET_STOP);

    if (WaitForValue(&worker->thread_status, THREAD_WORKER_STATUS_STOP, 1, MAX_RUNNING_TASK_TIME_MS, 1) == 0)
    {
        thread_join(worker->thread);
    }
    else
    {
        fprintf(stderr, "[thread_pool] DestroyThreadWorker: worker thread did not stop in time, force-killing\n");
        thread_destroy(worker->thread);
    }

    init_pool_worker_t* init_data = worker->pool->init_data;

    if (init_data->destroy_func)
    {
        init_data->destroy_func(worker);
    }

    free(worker);

    return;
}

static inline void* PoolThreadWorker(void* arg)
{
    pool_worker_t* worker = (pool_worker_t*)arg;

    int (*worker_function)(pool_worker_t*) = worker->pool->init_data->worker_func;

    if (!worker_function)
    {
        fprintf(stderr, "[thread_pool] PoolThreadWorker: worker_func is NULL\n");
        atomic_int_set(&worker->thread_status, THREAD_WORKER_STATUS_ERROR);
        return NULL; // no worker function defined
    }

    atomic_int_t* a_pool_order = &worker->pool_status;
    int pool_order = atomic_int_get(a_pool_order);
    atomic_int_t* a_status = &worker->thread_status;

    atomic_int_set(a_status, THREAD_WORKER_STATUS_IDLE);

    while (pool_order != THREAD_WORKER_SET_STOP)
    {
        if (pool_order == THREAD_WORKER_SET_OK)
        {
            goto while_end;
        }

        else if (pool_order == THREAD_WORKER_SET_DO_TASK)
        {
            worker->start_time = get_time_ms();

            // Mark task as running before calling the worker function
            atomic_int_set(&worker->task->job_status, TASK_STATUS_RUNNING);
            atomic_int_set(a_status, THREAD_WORKER_STATUS_WORKING);
            atomic_int_set(a_pool_order, THREAD_WORKER_SET_OK);

            if (worker_function(worker))
            {
                fprintf(stderr, "[thread_pool] PoolThreadWorker: worker_func returned error\n");
                atomic_int_set(&worker->task->job_status, TASK_STATUS_ERROR);
                atomic_int_set(a_status, THREAD_WORKER_STATUS_ERROR);
                return NULL; // error during work
            }

            atomic_int_set(a_status, THREAD_WORKER_STATUS_IDLE); // ready for next task
            worker->start_time = 0;
        }

        while_end:
        sleep_ms(THREAD_SLEEP_TIME_MS);
        pool_order = atomic_int_get(a_pool_order);
    }

    atomic_int_set(a_status, THREAD_WORKER_STATUS_STOP);
    return NULL;
}

//Pool functions

static inline pool_t* CreatePool(int num_threads, init_pool_worker_t* init_data)
{
    pool_t* pool = (pool_t*)calloc(1, sizeof(pool_t));
    if (!pool) { fprintf(stderr, "[thread_pool] CreatePool: calloc failed for pool\n"); return NULL; }

    atomic_int_set(&pool->status, POOL_STATUS_INIT);
    atomic_int_set(&pool->communication_status, POOL_SET_OK);
    atomic_int_set(&pool->free_to_add_task, 1);

    pool->task_to_add = NULL;
    pool->num_threads = num_threads;
    pool->init_data = init_data;
    pool->threads = (pool_worker_t**)calloc(num_threads, sizeof(pool_worker_t*));
    if (!pool->threads)
    {
        fprintf(stderr, "[thread_pool] CreatePool: calloc failed for threads array\n");
        goto free_all;
    }
    pool->queue_tasks = (task_t**)calloc(INITIAL_QUEUE_SIZE, sizeof(task_t*));
    if (!pool->queue_tasks)
    {
        fprintf(stderr, "[thread_pool] CreatePool: calloc failed for queue_tasks\n");
        goto free_all;
    }
    pool->queue_size = INITIAL_QUEUE_SIZE;

    pool->running_tasks = (task_t**)calloc(num_threads, sizeof(task_t*));
    if (!pool->running_tasks)
    {
        fprintf(stderr, "[thread_pool] CreatePool: calloc failed for running_tasks\n");
        goto free_all;
    }

    for (int i = 0; i < num_threads; i++)
    {
        pool->threads[i] = CreateThreadWorker(pool);
        if (!pool->threads[i])
        {
            fprintf(stderr, "[thread_pool] CreatePool: CreateThreadWorker failed for thread %d\n", i);
            // handle error, destroy already created workers
            for (int j = 0; j < i; j++)
            {
                DestroyThreadWorker(pool->threads[j]);
                pool->threads[j] = NULL;
            }
            goto free_all;
        }
    }

    pool->thread = (thread_t*)malloc(sizeof(thread_t));
    if (!pool->thread)
    {
        fprintf(stderr, "[thread_pool] CreatePool: malloc failed for pool->thread\n");
        goto free_all;
    }

    if (thread_create(pool->thread, PoolWorker, (void*)pool) != 0)
    {
        fprintf(stderr, "[thread_pool] CreatePool: thread_create failed for PoolWorker\n");
        goto free_all;
    }

    return pool;

    free_all:
    if (!pool) return NULL;
    if (pool->queue_tasks) free(pool->queue_tasks);
    if (pool->running_tasks) free(pool->running_tasks);
    if (pool->thread) free(pool->thread);
    if (pool->threads)
    {
        if (pool->threads[0]!= NULL)
        {
            for (int i = 0; i < num_threads; i++)
            {
                if (pool->threads[i])
                {
                    DestroyThreadWorker(pool->threads[i]);
                    pool->threads[i] = NULL;
                }
            }
        }
        free(pool->threads);
    }
    free(pool);
    return NULL;
}

static inline void DestroyPool(pool_t* pool)
{
    if (!pool) return;
    atomic_int_set(&pool->communication_status, POOL_SET_STOP);

    if (WaitForValue(&pool->status, POOL_STATUS_STOP, 1, MAX_POOL_SHUTDOWN_TIME_MS, 1) == 0)
    {
        thread_join(*pool->thread);
    }
    else
    {
        fprintf(stderr, "[thread_pool] DestroyPool: pool thread did not stop in time, force-killing\n");
        thread_destroy(*pool->thread);
    }

    for (int i = 0; i < pool->num_threads; i++)
    {
        if (pool->threads[i])
        {
            DestroyThreadWorker(pool->threads[i]);
            pool->threads[i] = NULL;
        }
    }

    if (pool->queue_tasks) free(pool->queue_tasks);
    if (pool->running_tasks) free(pool->running_tasks);
    if (pool->thread) free(pool->thread);
    if (pool->threads)
    {
        if (pool->threads[0]!= NULL)
        {
            for (int i = 0; i < pool->num_threads; i++)
            {
                if (pool->threads[i])
                {
                    DestroyThreadWorker(pool->threads[i]);
                    pool->threads[i] = NULL;
                }
            }
        }
        free(pool->threads);
    }
    free(pool);
    return;
}

static inline void* PoolWorker(void* arg)
{
    fprintf(stderr,"[thread_pool] PoolWorker: pool management thread started\n");
    pool_t* pool = (pool_t*)arg;

    atomic_int_t* a_status = &pool->status;

    atomic_int_set(a_status, POOL_STATUS_EMPTY);
    int status = atomic_int_get(&pool->communication_status);
    fprintf(stderr, "[thread_pool] PoolWorker: initial pool communication status: %d\n", status);
    while (status != POOL_SET_STOP)
    {
        switch (status)
        {
            case POOL_SET_CLEAR_TASKS:

                //clear tasks in queue
                atomic_int_set(a_status, POOL_STATUS_OK);
                for (int i = 0; i < pool->queue_size; i++)
                {
                    if (pool->queue_tasks[i])
                    {
                        DestroyTask(pool->queue_tasks[i]);
                        pool->queue_tasks[i] = NULL;
                    }
                }

                //clear tasks in add to queue
                if (atomic_int_get(&pool->free_to_add_task) == 0)
                {
                    int is_value = 0;
                    
                    while (is_value == 0)
                    {
                        task_t* task_to_add = pool->task_to_add;
                        pool->task_to_add = NULL;
                        DestroyTask(task_to_add);

                        atomic_int_set(&pool->free_to_add_task, 1);

                        is_value = WaitForValue(&pool->free_to_add_task, 0, 1, SLEEP_FOR_ADD_TASK_TIME_MS * 10, 1);
                    }
                }

                //clear tasks in workers
                for (int i = 0; i < pool->num_threads; i++)
                {
                    if (WaitForValue(&pool->threads[i]->thread_status, THREAD_WORKER_STATUS_WORKING, 1, MAX_RUNNING_TASK_TIME_MS, 0) == 0)
                    {
                        DestroyTask(pool->running_tasks[i]);
                        pool->running_tasks[i] = NULL;
                    }
                    else
                    {
                        DestroyThreadWorker(pool->threads[i]);
                        pool->threads[i] = CreateThreadWorker(pool);
                        DestroyTask(pool->running_tasks[i]);
                        pool->running_tasks[i] = NULL;
                    }
                    continue;
                }

                goto while_end;
            case POOL_SET_OK:
                //handle threads
                for (int worker_inx = 0, worker_status = 0; worker_inx < pool->num_threads; worker_inx++)
                {
                    worker_status = atomic_int_get(&pool->threads[worker_inx]->thread_status);
                    uint32_t worker_time = pool->threads[worker_inx]->start_time;

                    switch (worker_status)
                    {
                        case THREAD_WORKER_STATUS_IDLE:
                            // Guard against double-dispatch: if the pool already sent
                            // DO_TASK but the worker hasn't set itself to WORKING yet,
                            // it still looks IDLE. Skip it until it acknowledges.
                            if (atomic_int_get(&pool->threads[worker_inx]->pool_status)
                                    == THREAD_WORKER_SET_DO_TASK)
                                break;
                            if (atomic_int_get(a_status) != POOL_STATUS_EMPTY)
                            {
                                for (int task_inx = 0; task_inx < pool->queue_size; task_inx++)
                                {
                                    if (pool->queue_tasks[task_inx])
                                    {
                                        pool->threads[worker_inx]->task = pool->queue_tasks[task_inx];
                                        pool->running_tasks[worker_inx] = pool->queue_tasks[task_inx];
                                        pool->queue_tasks[task_inx] = NULL;

                                        atomic_int_set(&pool->threads[worker_inx]->pool_status, THREAD_WORKER_SET_DO_TASK);
                                        goto find_task;
                                    }
                                }

                                atomic_int_set(a_status, POOL_STATUS_EMPTY);

                                find_task: ;
                            }
                            break;
                        case THREAD_WORKER_STATUS_WORKING:
                            worker_status = atomic_int_get(&pool->threads[worker_inx]->thread_status);
                            if (worker_status != THREAD_WORKER_STATUS_WORKING)
                                break;
                            if (worker_time > get_time_ms() - MAX_RUNNING_TASK_TIME_MS)
                            {
                                break; // not yet timed out
                            }
                            //go to error status block (timed out)
                            __attribute__((fallthrough));
                        case THREAD_WORKER_STATUS_INIT:
                            fprintf(stderr, "[thread_pool] PoolWorker: worker thread %d timed out (status=%d), attempting recovery\n", worker_inx, worker_status);
                            sleep_ms(MAX_RUNNING_TASK_TIME_MS);
                            worker_status = atomic_int_get(&pool->threads[worker_inx]->thread_status);
                            if (worker_status != THREAD_WORKER_STATUS_INIT && worker_status != THREAD_WORKER_STATUS_WORKING) break;
                            //go to error status block
                            __attribute__((fallthrough));
                        case THREAD_WORKER_STATUS_ERROR:
                            DestroyThreadWorker(pool->threads[worker_inx]);
                            pool->threads[worker_inx] = CreateThreadWorker(pool);
                            if (!pool->threads[worker_inx]) { fprintf(stderr, "[thread_pool] PoolWorker: failed to recreate worker thread %d\n", worker_inx); goto fatal_error; }
                            pool->threads[worker_inx]->task = pool->running_tasks[worker_inx];
                            atomic_int_set(&pool->threads[worker_inx]->pool_status, THREAD_WORKER_SET_DO_TASK);
                            sleep_ms(MAX_RUNNING_TASK_TIME_MS);
                            if (WaitForValue(&pool->threads[worker_inx]->thread_status, THREAD_WORKER_STATUS_IDLE, 1, MAX_RUNNING_TASK_TIME_MS, 1) != 0)
                            {
                                fprintf(stderr, "[thread_pool] PoolWorker: recovered worker thread %d failed to become idle\n", worker_inx);
                                DestroyTask(pool->running_tasks[worker_inx]);
                                pool->running_tasks[worker_inx] = NULL;
                                goto fatal_error;
                            }
                            break;
                        default:
                            break;
                    }
                }
                
                //get data from add to  func to queue
                int free_to_add = atomic_int_get(&pool->free_to_add_task);
                if (free_to_add == 0)
                {
                    //int is_value = 0;
                    
                    //while (is_value == 0)
                    //{
                        task_t* task_to_add = pool->task_to_add;
                        pool->task_to_add = NULL;

                        for (int i = 0; i < pool->queue_size; i++)
                        {
                            if (!pool->queue_tasks[i])
                            {
                                pool->queue_tasks[i] = task_to_add;
                                goto task_add;
                            }
                        }

                        //no free space in queue
                        int old_size = pool->queue_size;
                        void* temp_ptr = (void*)realloc(pool->queue_tasks, old_size * 2 * sizeof(task_t*));
                        if (temp_ptr == NULL)
                        {
                            fprintf(stderr, "[thread_pool] PoolWorker: realloc failed for queue_tasks (current size=%d)\n", old_size);
                            goto fatal_error;
                        }

                        pool->queue_tasks = (task_t**)temp_ptr;
                        memset(&pool->queue_tasks[old_size], 0, old_size * sizeof(task_t*));
                        pool->queue_size = old_size * 2;

                        //another try
                        for (int i = 0; i < pool->queue_size; i++)
                        {
                            if (!pool->queue_tasks[i])
                            {
                                pool->queue_tasks[i] = task_to_add;
                                goto task_add;
                            }
                        }

                        task_add:
                        atomic_int_cas(&pool->free_to_add_task, 0, 1);
                        atomic_int_set(a_status, POOL_STATUS_OK);

                        //is_value = WaitForValue(&pool->free_to_add_task, 0, 1, SLEEP_FOR_ADD_TASK_TIME_MS * 10, 1);
                    //}
                }

                break;
            default:
                break;

        }


        while_end:
        sleep_ms(POOL_LOOP_TIMEOUT_MS);
        status = atomic_int_get(&pool->communication_status);
    }

    atomic_int_set(&pool->status, POOL_STATUS_STOP);
    return NULL;

    fatal_error:
    fprintf(stderr, "[thread_pool] PoolWorker: fatal error, pool entering error state\n");
    atomic_int_set(a_status, POOL_STATUS_ERROR);
    return NULL;

}

static inline int AddTaskToPool(pool_t* pool, task_t* task)
{
    if (atomic_int_get(&pool->communication_status) == POOL_SET_STOP)
    {
        fprintf(stderr, "[thread_pool] AddTaskToPool: pool is stopping, cannot add task\n");
        return -1; // pool is stopping, cannot add task
    }

    uint32_t start_time = get_time_ms();

    while (get_time_ms() - start_time < MAX_TIME_FOR_ADD_TASK)
    {
        if (atomic_int_cas(&pool->free_to_add_task, 1, 2)) // 2 = writing in progress
        {
            pool->task_to_add = task;
            atomic_int_set(&task->job_status, TASK_STATUS_IN_QUEUE);
            atomic_int_set(&pool->free_to_add_task, 0); // 0 = task ready for pool
            return 0;
        }
        sleep_ms(SLEEP_FOR_ADD_TASK_TIME_MS);
    }
    
    
    fprintf(stderr, "[thread_pool] AddTaskToPool: timeout waiting to add task\n");
    return -1; // timeout
}

static inline int ClearTasksInPool(pool_t* pool)
{
    if (atomic_int_get(&pool->communication_status) == POOL_SET_STOP)
    {
        fprintf(stderr, "[thread_pool] ClearTasksInPool: pool is stopping, cannot clear tasks\n");
        return -1; // pool is stopping, cannot clear tasks
    }

    atomic_int_set(&pool->communication_status, POOL_SET_CLEAR_TASKS);

    if (WaitForValue(&pool->status, POOL_STATUS_EMPTY, 1, MAX_POOL_SHUTDOWN_TIME_MS + MAX_RUNNING_TASK_TIME_MS, 1) != 0)
    {
        fprintf(stderr, "[thread_pool] ClearTasksInPool: timeout waiting for all tasks to clear\n");
        return -1; // timeout waiting for tasks to clear
    }

    return 0;
}

static inline int WaitForFinishPool(pool_t* pool)
{
    if (pool == NULL) { fprintf(stderr, "[thread_pool] WaitForFinishPool: pool is NULL\n"); return -1; }
    if (atomic_int_get(&pool->communication_status) == POOL_SET_STOP)
    {
        fprintf(stderr, "[thread_pool] WaitForFinishPool: pool is stopping\n");
        return -1;
    }

    int ret = WaitForValue(&pool->status, POOL_STATUS_EMPTY, 1,
                           MAX_POOL_SHUTDOWN_TIME_MS + MAX_RUNNING_TASK_TIME_MS, 1);
    if (ret != 0) fprintf(stderr, "[thread_pool] WaitForFinishPool: timeout waiting for pool to finish\n");
    return ret;
}