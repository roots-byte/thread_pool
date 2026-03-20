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
            return -2;
        }

        if (atomic)
            current_value = atomic_int_get((atomic_int_t*)variable);
        else
            current_value = *(int*)variable;

        if (match && current_value == value)
            return 0;
        else if (!match && current_value != value)
            return 0;

        sleep_ms(WAIT_FOR_VALUE_SLEEP_MS);
        elapsed = get_time_ms() - start_time;
        if (elapsed < 0)
        {
            start_time = get_time_ms();
            elapsed = 0;
        }
    }

    return -1;
}

//Queue functions
static inline task_queue_t* CreateTaskQueue(void)
{
    task_queue_t* queue = (task_queue_t*)malloc(sizeof(task_queue_t));
    if (!queue) { fprintf(stderr, "[thread_pool] CreateTaskQueue: malloc failed\n"); return NULL; }
    
    atomic_int_set(&queue->capacity, QUEUE_CAPACITY);
    queue->tasks = (atomic_ptr_t*)calloc(QUEUE_CAPACITY, sizeof(atomic_ptr_t));
    if (!queue->tasks) { fprintf(stderr, "[thread_pool] CreateTaskQueue: malloc failed\n"); free(queue); return NULL; }
    atomic_int_set(&queue->head, 0);
    atomic_int_set(&queue->tail, 0);
    atomic_int_set(&queue->running, 1);
    mutex_init(&queue->mutex);
    cond_init(&queue->cond);


    return queue;
}

static inline void DestroyTaskQueue(task_queue_t* queue)
{
    if (queue == NULL) return;

    cond_destroy(&queue->cond);
    mutex_destroy(&queue->mutex);

    if (queue->tasks)
    {
        for (int i = atomic_int_get(&queue->tail); i != atomic_int_get(&queue->head); i = (i + 1) % atomic_int_get(&queue->capacity))
        {
            void* ptr = atomic_ptr_get(&queue->tasks[i]);
            if (ptr)
                DestroyTask((task_t*)ptr);
        }
        free(queue->tasks);
    }
    free(queue);

    queue = NULL;
}

static inline int ResizeTaskQueue(task_queue_t* queue)
{
    if (queue == NULL || queue->tasks == NULL)
    {
        fprintf(stderr, "[thread_pool] ResizeTaskQueue: queue or tasks is NULL\n");
        return -1;
    }

    int old_capacity = atomic_int_get(&queue->capacity);
    int new_capacity = old_capacity * 2;
    atomic_ptr_t* new_tasks = (atomic_ptr_t*)calloc(new_capacity, sizeof(atomic_ptr_t));
    if (!new_tasks)
    {
        fprintf(stderr, "[thread_pool] ResizeQueue: malloc failed\n");
        return -1;
    }

    int head = atomic_int_get(&queue->head);
    int tail = atomic_int_get(&queue->tail);
    int size = (head >= tail) ? (head - tail) : (old_capacity - tail + head);

    for (int i = 0; i < size; i++)
    {
        int index = (tail + i) % old_capacity;
        void* task = atomic_ptr_get(&queue->tasks[index]);
        atomic_ptr_set(&new_tasks[i], task);
    }

    free(queue->tasks);
    queue->tasks = new_tasks;
    atomic_int_set(&queue->capacity, new_capacity);
    atomic_int_set(&queue->head, size);
    atomic_int_set(&queue->tail, 0);

    return 0;
}

static inline int EnqueueTask(task_queue_t* queue, task_t* task)
{
    if (queue == NULL || queue->tasks == NULL || task == NULL)
    {
        fprintf(stderr, "[thread_pool] EnqueueTask: queue/tasks/task is NULL\n");
        return -1;
    }

    if (!atomic_int_get(&queue->running))
    {
        return -1;
    }
    mutex_lock(&queue->mutex);
    int size = atomic_int_get(&queue->capacity);
    int head = atomic_int_get(&queue->head);
    int tail = atomic_int_get(&queue->tail);

    if ((head + 1) % size == tail)
    {
        if (ResizeTaskQueue(queue) != 0)
        {
            mutex_unlock(&queue->mutex);
            return -1; // failed to resize
        }
        head = atomic_int_get(&queue->head);
        tail = atomic_int_get(&queue->tail);
        size = atomic_int_get(&queue->capacity);
    }

    atomic_ptr_set(&queue->tasks[head], task);
    head = (head + 1) % size;
    atomic_int_set(&queue->head, head);
    task->ts_queued = get_time_ms();
    atomic_int_set(&task->job_status, TASK_STATUS_IN_QUEUE);
    mutex_unlock(&queue->mutex);

    cond_signal(&queue->cond);
    return 0;

}

static inline task_t* DequeueTask(task_queue_t* queue)
{
    if (queue == NULL || queue->tasks == NULL)
    {
        fprintf(stderr, "[thread_pool] DequeueTask: queue or tasks is NULL\n");
        return NULL;
    }

    if (!atomic_int_get(&queue->running))
    {
        return NULL;
    }

    mutex_lock(&queue->mutex);

    int tail = atomic_int_get(&queue->tail);
    int size = atomic_int_get(&queue->capacity);

    void* task = atomic_ptr_set(&queue->tasks[tail], NULL);
    if (task != NULL)
    {
        ((task_t*)task)->ts_dispatch = get_time_ms();
        tail = (tail + 1) % size;
        atomic_int_set(&queue->tail, tail);
        mutex_unlock(&queue->mutex);
        return (task_t*)task; // successfully dequeued
    }
        
    mutex_unlock(&queue->mutex);

    cond_broadcast(&queue->cond);

    return NULL;
}


//Task functions

static inline task_t* CreateTask(void)
{
    task_t* task = (task_t*)malloc(sizeof(task_t));
    if (!task) { fprintf(stderr, "[thread_pool] CreateTask: malloc failed\n"); return NULL; }

    atomic_int_set(&task->job_status, TASK_STATUS_CREATED);
    task->task_id = 0;
    task->num_samples = 0;
    task->hits = 0;
    task->ts_submit = 0;
    task->ts_queued = 0;
    task->ts_dispatch = 0;
    task->ts_start = 0;
    task->ts_finish = 0;
    return task;
}

static inline void DestroyTask(task_t* task)
{
    if (task == NULL) return;

    int status = atomic_int_get(&task->job_status);

    if (!atomic_int_cas(&task->job_status, TASK_STATUS_IN_QUEUE, TASK_STATUS_STOP))
    {
        WaitForValue(&task->job_status, TASK_STATUS_RUNNING, 1, MAX_RUNNING_TASK_TIME_MS, 0);
        atomic_int_set(&task->job_status, TASK_STATUS_STOP);
    }

    free(task);

}

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
    if (pool == NULL || pool->init_data == NULL)
    {
        fprintf(stderr, "[thread_pool] CreateThreadWorker: pool or init_data is NULL\n");
        return NULL;
    }

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
        return NULL;
    }

    if (init_data->init_func)
    {
        if (init_data->init_func(init_data, worker))
        {
            fprintf(stderr, "[thread_pool] CreateThreadWorker: init_func failed\n");
            free(worker);
            return NULL;
        }
    }

    if (thread_create(&worker->thread, PoolThreadWorker, (void*)worker) != 0)
    {
        fprintf(stderr, "[thread_pool] CreateThreadWorker: thread_create failed\n");
        free(worker);
        return NULL;
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
    if (worker == NULL)
    {
        return;
    }

    if (worker->pool == NULL)
    {
        fprintf(stderr, "[thread_pool] DestroyThreadWorker: worker->pool is NULL\n");
        free(worker);
        return;
    }

    atomic_int_set(&worker->pool_status, THREAD_WORKER_SET_STOP);

    if (worker->pool->queue != NULL)
    {
        cond_broadcast(&worker->pool->queue->cond);
    }

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

    if (init_data && init_data->destroy_func)
    {
        init_data->destroy_func(worker);
    }

    free(worker);

    return;
}

static inline void* PoolThreadWorker(void* arg)
{
    pool_worker_t* worker = (pool_worker_t*)arg;

    if (worker == NULL || worker->pool == NULL || worker->pool->init_data == NULL || worker->pool->queue == NULL)
    {
        fprintf(stderr, "[thread_pool] PoolThreadWorker: invalid worker/pool/init_data/queue\n");
        return NULL;
    }

    int (*worker_function)(pool_worker_t*) = worker->pool->init_data->worker_func;

    if (!worker_function)
    {
        fprintf(stderr, "[thread_pool] PoolThreadWorker: worker_func is NULL\n");
        atomic_int_set(&worker->thread_status, THREAD_WORKER_STATUS_ERROR);
        return NULL;
    }

    atomic_int_t* a_pool_order = &worker->pool_status;
    atomic_int_t* a_status = &worker->thread_status;

    atomic_int_set(a_status, THREAD_WORKER_STATUS_IDLE);

    while (atomic_int_get(a_pool_order) != THREAD_WORKER_SET_STOP)
    {
        if ((worker->task = DequeueTask(worker->pool->queue)) != NULL)
        {
            worker->start_time = get_time_ms();

            if (!atomic_int_cas(&worker->task->job_status, TASK_STATUS_IN_QUEUE,TASK_STATUS_RUNNING))
            {
                continue; // task was stolen by another worker or is being destroyed, skip it
            }

            atomic_int_set(a_status, THREAD_WORKER_STATUS_WORKING);
            atomic_int_set(a_pool_order, THREAD_WORKER_SET_OK);
            worker->task->ts_start = worker->start_time;

            if (worker_function(worker))
            {
                fprintf(stderr, "[thread_pool] PoolThreadWorker: worker_func returned error\n");
                atomic_int_set(&worker->task->job_status, TASK_STATUS_ERROR);
                atomic_int_set(a_status, THREAD_WORKER_STATUS_ERROR);
                return NULL;
            }

            atomic_int_set(a_status, THREAD_WORKER_STATUS_IDLE);
            worker->start_time = 0;
            atomic_int_set(&worker->task->job_status, TASK_STATUS_FINISHED);
            worker->task = NULL;
            continue;
        }
        else
        {
            sleep_ms(1);
            continue;
        }
    }

    atomic_int_set(a_status, THREAD_WORKER_STATUS_STOP);
    return NULL;
}

//Pool functions

static inline pool_t* CreatePool(int num_threads, init_pool_worker_t* init_data)
{
    if (num_threads <= 0 || init_data == NULL || init_data->worker_func == NULL)
    {
        fprintf(stderr, "[thread_pool] CreatePool: invalid arguments\n");
        return NULL;
    }

    pool_t* pool = (pool_t*)calloc(1, sizeof(pool_t));
    if (!pool) { fprintf(stderr, "[thread_pool] CreatePool: calloc failed for pool\n"); return NULL; }

    atomic_int_set(&pool->status, POOL_STATUS_INIT);
    atomic_int_set(&pool->communication_status, POOL_SET_OK);

    pool->num_threads = num_threads;
    pool->init_data = init_data;
    pool->threads = (pool_worker_t**)calloc(num_threads, sizeof(pool_worker_t*));
    if (!pool->threads)
    {
        fprintf(stderr, "[thread_pool] CreatePool: calloc failed for threads array\n");
        goto free_all;
    }
   
    pool->queue = CreateTaskQueue();
    if (!pool->queue)
    {
        fprintf(stderr, "[thread_pool] CreatePool: CreateTaskQueue failed\n");
        goto free_all;
    }

    pool->running_tasks = (atomic_ptr_t*)calloc(num_threads, sizeof(atomic_ptr_t));
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
    if (pool->queue) DestroyTaskQueue(pool->queue);
    if (pool->running_tasks) free(pool->running_tasks);
    if (pool->thread) free(pool->thread);
    if (pool->threads) free(pool->threads);
    free(pool);
    return NULL;
}

static inline void DestroyPool(pool_t* pool)
{
    if (!pool) return;

    if (pool->thread == NULL)
    {
        fprintf(stderr, "[thread_pool] DestroyPool: pool->thread is NULL\n");
    }

    atomic_int_set(&pool->communication_status, POOL_SET_STOP);

    if (pool->thread && WaitForValue(&pool->status, POOL_STATUS_STOP, 1, MAX_POOL_SHUTDOWN_TIME_MS, 1) == 0)
    {
        thread_join(*pool->thread);
    }
    else if (pool->thread)
    {
        fprintf(stderr, "[thread_pool] DestroyPool: pool thread did not stop in time, force-killing\n");
        thread_destroy(*pool->thread);
    }

    for (int i = 0; pool->threads && i < pool->num_threads; i++)
    {
        if (pool->threads[i])
        {
            DestroyThreadWorker(pool->threads[i]);
            pool->threads[i] = NULL;
        }
    }

    if (pool->queue) DestroyTaskQueue(pool->queue);
    if (pool->running_tasks) free(pool->running_tasks);
    if (pool->thread) free(pool->thread);
    if (pool->threads) free(pool->threads);
    free(pool);
    return;
}

static inline void* PoolWorker(void* arg)
{
    pool_t* pool = (pool_t*)arg;

    if (pool == NULL || pool->queue == NULL || pool->threads == NULL || pool->running_tasks == NULL)
    {
        fprintf(stderr, "[thread_pool] PoolWorker: invalid pool internals\n");
        return NULL;
    }

    atomic_int_t* a_status = &pool->status;

    atomic_int_set(a_status, POOL_STATUS_EMPTY);
    int status = atomic_int_get(&pool->communication_status);


    while (status != POOL_SET_STOP)
    {
        switch (status)
        {
            case POOL_SET_CLEAR_TASKS:

                //clear tasks in queue
                atomic_int_set(a_status, POOL_STATUS_OK);
                atomic_int_set(&pool->queue->running, 0);

                for (int i = atomic_int_get(&pool->queue->tail); i != atomic_int_get(&pool->queue->head); i = (i + 1) % atomic_int_get(&pool->queue->capacity))
                {
                    task_t* task = atomic_ptr_set(&pool->queue->tasks[i], NULL);
                    if (task)
                    {
                        DestroyTask(task);
                    }
                }

                //clear tasks in workers
                for (int i = 0; i < pool->num_threads; i++)
                {
                    if (WaitForValue(&pool->threads[i]->thread_status, THREAD_WORKER_STATUS_WORKING, 1, MAX_RUNNING_TASK_TIME_MS, 0) == 0)
                    {
                        task_t* running_task = atomic_ptr_set(&pool->running_tasks[i], NULL);
                        if (running_task)
                        {
                            DestroyTask(running_task);
                        }
                        atomic_int_set(&pool->threads[i]->pool_status, THREAD_WORKER_SET_OK);

                    }
                    else
                    {
                        task_t* running_task = atomic_ptr_set(&pool->running_tasks[i], NULL);
                        DestroyThreadWorker(pool->threads[i]);
                        pool->threads[i] = CreateThreadWorker(pool);
                        if (running_task)
                        {
                            DestroyTask(running_task);
                        }
                        if (pool->threads[i])
                            atomic_int_set(&pool->threads[i]->pool_status, THREAD_WORKER_SET_OK);
                    }
                    continue;
                }

                atomic_int_set(&pool->communication_status, POOL_SET_OK);
                atomic_int_set(a_status, POOL_STATUS_EMPTY);
                goto while_end;
            case POOL_SET_OK:
            {

                for (int worker_inx = 0, worker_status = 0; worker_inx < pool->num_threads; worker_inx++)
                {
                    worker_status = atomic_int_get(&pool->threads[worker_inx]->thread_status);
                    uint32_t worker_time = pool->threads[worker_inx]->start_time;

                    switch (worker_status)
                    {
                        case THREAD_WORKER_STATUS_IDLE:
                            break;
                        case THREAD_WORKER_STATUS_WORKING:
                            worker_status = atomic_int_get(&pool->threads[worker_inx]->thread_status);
                            if (worker_status != THREAD_WORKER_STATUS_WORKING)
                                break;
                            if (worker_time == 0 || (uint32_t)(get_time_ms() - worker_time) < (uint32_t)MAX_RUNNING_TASK_TIME_MS)
                            {
                                break; // not yet timed out
                            }
                            //go to error status block (timed out)
                            #ifndef _WIN32
                            __attribute__((fallthrough));
                            #endif
                        case THREAD_WORKER_STATUS_INIT:
                            fprintf(stderr, "[thread_pool] PoolWorker: worker thread %d timed out (status=%d), attempting recovery\n", worker_inx, worker_status);
                            //go to error status block
                            #ifndef _WIN32
                            __attribute__((fallthrough));
                            #endif
                        case THREAD_WORKER_STATUS_ERROR:
                            fprintf(stderr, "[thread_pool] PoolWorker: worker thread %d in error state, attempting recovery\n", worker_inx);
                            DestroyThreadWorker(pool->threads[worker_inx]);
                            pool->threads[worker_inx] = CreateThreadWorker(pool);
                            if (!pool->threads[worker_inx]) { fprintf(stderr, "[thread_pool] PoolWorker: failed to recreate worker thread %d\n", worker_inx); goto fatal_error; }
                            if (WaitForValue(&pool->threads[worker_inx]->thread_status, THREAD_WORKER_STATUS_INIT, 1, MAX_RUNNING_TASK_TIME_MS, 1) != 0)
                            {
                                fprintf(stderr, "[thread_pool] PoolWorker: recreated worker thread %d did not initialize in time\n", worker_inx);
                                DestroyThreadWorker(pool->threads[worker_inx]);
                                pool->threads[worker_inx] = NULL;
                                goto fatal_error;
                            }
                            if (atomic_ptr_get(&pool->running_tasks[worker_inx]) != NULL)
                            {   
                                pool->threads[worker_inx]->task = atomic_ptr_get(&pool->running_tasks[worker_inx]);
                                atomic_int_set(&pool->threads[worker_inx]->pool_status, THREAD_WORKER_SET_DO_TASK);
                                if (WaitForValue(&pool->threads[worker_inx]->thread_status, THREAD_WORKER_STATUS_IDLE, 1, MAX_RUNNING_TASK_TIME_MS, 1) != 0)
                                {
                                    fprintf(stderr, "[thread_pool] PoolWorker: recovered worker thread %d failed to become idle\n", worker_inx);
                                    DestroyTask((task_t*)atomic_ptr_get(&pool->running_tasks[worker_inx]));
                                    atomic_ptr_set(&pool->running_tasks[worker_inx], NULL);
                                    goto fatal_error;
                                }
                            }
                            break;
                        default:
                            break;
                    }
                }

                break;
            } // end POOL_SET_OK
            default:
                break;

        }


        while_end:
        sleep_ms(POOL_LOOP_TIMEOUT_MS);
        status = atomic_int_get(&pool->communication_status);
    } // end while

    atomic_int_set(&pool->status, POOL_STATUS_STOP);
    return NULL;

    fatal_error:
    fprintf(stderr, "[thread_pool] PoolWorker: fatal error, pool entering error state\n");
    atomic_int_set(a_status, POOL_STATUS_ERROR);
    return NULL;

}

static inline int AddTaskToPool(pool_t* pool, task_t* task)
{
    if (pool == NULL || task == NULL || pool->queue == NULL)
    {
        fprintf(stderr, "[thread_pool] AddTaskToPool: invalid pool/task/queue\n");
        return -1;
    }

    if (atomic_int_get(&pool->communication_status) == POOL_SET_STOP)
    {
        fprintf(stderr, "[thread_pool] AddTaskToPool: pool is stopping, cannot add task\n");
        return -1;
    }

    task->ts_submit = get_time_ms();

    if (EnqueueTask(pool->queue, task) == 0)
    {
        return 0;
    }
    fprintf(stderr, "[thread_pool] AddTaskToPool: timeout waiting to add task\n");
    return -1;
}

static inline int ClearTasksInPool(pool_t* pool)
{
    if (pool == NULL || pool->queue == NULL || pool->threads == NULL || pool->running_tasks == NULL)
    {
        fprintf(stderr, "[thread_pool] ClearTasksInPool: invalid pool internals\n");
        return -1;
    }

    if (atomic_int_get(&pool->communication_status) == POOL_SET_STOP)
    {
        fprintf(stderr, "[thread_pool] ClearTasksInPool: pool is stopping, cannot clear tasks\n");
        return -1;
    }

    atomic_int_set(&pool->communication_status, POOL_SET_CLEAR_TASKS);

    if (WaitForValue(&pool->status, POOL_STATUS_EMPTY, 1, MAX_POOL_SHUTDOWN_TIME_MS + MAX_RUNNING_TASK_TIME_MS, 1) != 0)
    {
        fprintf(stderr, "[thread_pool] ClearTasksInPool: timeout waiting for all tasks to clear\n");
        return -1;
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