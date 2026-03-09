# thread_pool

A lightweight, portable C thread pool with a dynamic task queue, automatic worker recovery, and a simple extension model. Works on Linux, macOS and Windows (POSIX threads / Win32 threads).

---

## Features

- **Fixed worker threads** — N threads stay alive for the lifetime of the pool; no spawn overhead per task.
- **Dynamic queue** — the task queue starts at `INITIAL_QUEUE_SIZE` slots and doubles automatically when full.
- **Worker recovery** — if a worker thread hangs beyond `MAX_RUNNING_TASK_TIME_MS` or exits with an error, the supervisor recreates it and retries the task.
- **Staged task waiting** — `WaitForFinishTask` tracks each stage of the task lifecycle (queued → running → finished) with independent per-stage timeouts.
- **Extensible structs** — add your own input/output fields to `task_t`, per-thread state to `pool_worker_t`, and shared config to `init_pool_worker_t` without touching the library code.
- **C++/Windows compatible** — wrapped in `extern "C"` and uses a thin `thread_pool_wrapper.h` abstraction over pthreads / Win32 threads.
- **No external dependencies** — only the C standard library and pthreads (or Win32 threads).

---

## How it works

```
  caller thread(s)
       │
       │  AddTaskToPool()
       ▼
  [ task_to_add ]  ──►  PoolWorker  (supervisor thread)
                               │
                     places into queue_tasks[]
                               │
               dispatches to idle pool_worker_t threads
                               │
                     worker calls worker_func(worker)
```

One supervisor thread (`PoolWorker`) owns the queue and all bookkeeping. N worker threads (`PoolThreadWorker`) sit idle waiting for the supervisor to assign a task.  
The caller communicates with the supervisor through a single atomic CAS handoff slot — no mutex needed on the hot path.

---

## Usage

### 1 — Extend the structs

In `thread_pool.h`, find the `>>>` marker comments and add your fields:

```c
typedef struct {
    atomic_int_t job_status;   // internal — do not touch
    atomic_int_t task_status;  // internal — do not touch
    uint32_t     add_time;     // internal — do not touch

    // >>> your task input/output fields <<<
    int    my_input;
    double my_result;
} task_t;

typedef struct pool_worker_t {
    // ... internal fields ...

    // >>> your per-thread state <<<
    unsigned int rand_seed;
} pool_worker_t;

typedef struct init_pool_worker_t {
    int  (*init_func)   (init_pool_worker_t*, pool_worker_t*);
    int  (*worker_func) (pool_worker_t*);
    void (*destroy_func)(pool_worker_t*);

    // >>> your shared pool-wide config <<<
    const char* output_dir;
} init_pool_worker_t;
```

### 2 — Implement the worker function

```c
int my_worker(pool_worker_t* worker)
{
    task_t* task = worker->task;

    // do the work
    task->my_result = heavy_computation(task->my_input);

    // REQUIRED: signal completion
    atomic_int_set(&task->job_status, TASK_STATUS_FINISHED);
    return 0;   // non-zero = error, pool will recreate this worker
}
```

> **Important:** `worker_func` **must** set `job_status = TASK_STATUS_FINISHED` before returning 0.  
> If it returns non-zero the task is marked as `TASK_STATUS_ERROR` and the worker thread is killed and recreated.

### 3 — Create the pool

```c
// Include the implementation directly (all functions are static inline)
#include "thread_pool.c"

init_pool_worker_t init = {
    .init_func    = my_init,    // optional — NULL if not needed
    .worker_func  = my_worker,  // required
    .destroy_func = my_cleanup, // optional — NULL if not needed
};

pool_t* pool = CreatePool(4, &init);
if (!pool) { /* handle error */ }
```

### 4 — Submit tasks and wait

```c
task_t* tasks[100];

for (int i = 0; i < 100; i++) {
    tasks[i] = CreateTask();
    tasks[i]->my_input = i;
    AddTaskToPool(pool, tasks[i]);
}

// Option A: wait for individual tasks in submission order
for (int i = 0; i < 100; i++) {
    WaitForFinishTask(tasks[i]);
    printf("result[%d] = %f\n", i, tasks[i]->my_result);
    DestroyTask(tasks[i]);
}

// Option B: wait for the whole pool to drain, then clean up
WaitForFinishPool(pool);
```

### 5 — Clean up

```c
DestroyPool(pool);
```

---

## API reference

### Task

| Function | Description |
|---|---|
| `CreateTask()` | Allocates a new task (`TASK_STATUS_CREATED`). Populate your fields, then call `AddTaskToPool`. |
| `DestroyTask(task)` | Waits for any in-flight execution to finish, then frees the task. |
| `WaitForFinishTask(task)` | Waits through each lifecycle stage (queued → running → done). Returns 0 on success, -1 on timeout. |

### Pool

| Function | Description |
|---|---|
| `CreatePool(n, init)` | Creates a pool with `n` worker threads and one supervisor thread. Waits for all workers to become ready before returning. |
| `DestroyPool(pool)` | Signals shutdown, joins all threads, frees all memory. |
| `AddTaskToPool(pool, task)` | Submits a task via atomic CAS handoff. Returns 0 on success, -1 on timeout or if pool is stopping. |
| `WaitForFinishPool(pool)` | Blocks until queue is empty and all workers are idle. |
| `ClearTasksInPool(pool)` | Discards all queued tasks and waits for running ones to finish. |

---

## Tuning constants

Define any of these **before** including `thread_pool.h` to override the defaults.

| Constant | Default | Description |
|---|---|---|
| `WAIT_FOR_VALUE_SLEEP_MS` | 2 ms | Poll sleep in all waiting loops. |
| `MAX_RUNNING_TASK_TIME_MS` | 10 000 ms | Max time a task may run before the worker is considered hung. |
| `MAX_TIME_IN_QUEUE_TIME_MS` | 10 000 ms | Max time a task may wait in the queue (`WaitForFinishTask` stage timeout). |
| `THREAD_SLEEP_TIME_MS` | 2 ms | Idle sleep of worker threads. |
| `POOL_LOOP_TIMEOUT_MS` | 1 ms | Supervisor main loop sleep — lower = faster task pickup latency. |
| `MAX_POOL_SHUTDOWN_TIME_MS` | 2 000 ms | Time `DestroyPool` waits for graceful stop. |
| `SLEEP_FOR_ADD_TASK_TIME_MS` | 2 ms | Retry sleep in `AddTaskToPool`. |
| `MAX_TIME_FOR_ADD_TASK` | 10 000 ms | Total timeout for `AddTaskToPool`. |
| `INITIAL_QUEUE_SIZE` | 100 | Initial task queue capacity (doubles automatically when full). |

---

## Example — Monte Carlo π estimation

The `example/` directory contains a self-contained demonstration: N worker threads each throw random points into the unit square and count how many land inside the unit circle. Results are accumulated across all tasks to estimate π.

```
cd example
make
./pi
```

```
=== Monte Carlo Pi Estimation (Thread Pool Demo) ===
  Threads  : 20
  Tasks    : 80
  Samples  : 3000000 per task  (240 M total)

  [################################] 80/80  ~Pi = 3.1415090  (240 M samples)

  Final estimate :  3.141508983
  Real Pi        :  3.141592654
  Absolute error :  8.367e-05  (0.00266%)
  Wall time      :  627 ms
```

The example also shows how to:
- extend `task_t` with `task_id`, `num_samples`, `hits`
- extend `pool_worker_t` with a per-thread `rand_seed`
- implement `init_func` to seed each worker with a unique random seed
- collect results per-task via `WaitForFinishTask` with a live progress bar

---

## File structure

```
thread_pool_wrapper.h   Platform abstraction (threads, atomics, sleep, time)
thread_pool.h           Public API — structs, constants, function declarations
thread_pool.c           Implementation (all static inline — include directly)
example/
  test.c                Monte Carlo π demonstration
  thread_pool.c         Copy of the implementation (self-contained)
  thread_pool.h         Copy of the header (self-contained)
  thread_pool_wrapper.h Copy of the wrapper (self-contained)
  Makefile
```

---

## Building

The library is header/source only — no separate build step. Include `thread_pool.c` directly in one translation unit:

```c
#include "thread_pool.c"
```

Compile with pthreads:

```sh
gcc -O2 -o my_program my_program.c -lpthread
```

---

## License

This project is licensed under the MIT License.  
See the [LICENSE](LICENSE) file for details.
