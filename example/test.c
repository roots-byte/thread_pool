/*
 * Monte Carlo Pi Estimation — Thread Pool Example
 * ================================================
 *
 * Each task throws `num_samples` random points into the unit square [0,1]x[0,1]
 * and counts how many land inside the quarter-unit circle (x²+y² ≤ 1).
 *
 *   Pi ≈ 4 × (total hits) / (total samples)
 *
 * The estimate converges visually as each task completes and results accumulate.
 *
 * Build:
 *   make
 * or:
 *   gcc -O2 -o pi test.c -lpthread -lm
 */

#include "thread_pool.c"

#include <stdio.h>
#include <stdlib.h>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#include <math.h>
#include <time.h>
#include <string.h>

#ifdef _WIN32
#include <io.h>
static int g_vt_enabled = 0;
static void enable_vt_mode(void)
{
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    if (hOut == INVALID_HANDLE_VALUE) return;
    DWORD mode = 0;
    if (!GetConsoleMode(hOut, &mode)) return;
    if (SetConsoleMode(hOut, mode | 0x0004 /*ENABLE_VIRTUAL_TERMINAL_PROCESSING*/))
        g_vt_enabled = 1;
}
#else
static int g_vt_enabled = 1; /* assume terminal supports ANSI */
static void enable_vt_mode(void) {}
#endif

// ─── Configuration ───────────────────────────────────────────────────

static int get_num_cpus(int multiplier)
{
    int cpus;
#ifdef _WIN32
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    cpus = (int)si.dwNumberOfProcessors;
#else
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    cpus = (n > 0) ? (int)n : 1;
#endif
    if (multiplier < 1) multiplier = 1;
    return cpus * multiplier;
}

#define THREAD_MULTIPLIER 1
#define NUM_TASKS         1000
#define SAMPLES_PER_TASK  1000000ULL

#define PROGRESS_POLL_SLEEP_MS 5

// ─── Worker function ─────────────────────────────────────────────────

static int monte_carlo_worker(pool_worker_t* worker)
{
    task_t* task = worker->task;

    // Mix task id into the seed so each task explores a different sequence
    unsigned int seed = worker->rand_seed ^ (unsigned int)task->task_id;
    if (seed == 0) seed = 1; // xorshift must not start at 0

    // Integer-only Monte Carlo: generate x,y in [0, 2^15),
    // check x*x + y*y <= (2^15)^2 = 2^30.  All fits in uint32_t.
    const uint32_t R2 = (1u << 30); // (2^15)^2

    uint64_t hits = 0;
    for (uint64_t i = 0; i < task->num_samples; i++)
    {
        seed ^= seed << 13;
        seed ^= seed >> 17;
        seed ^= seed << 5;
        uint32_t x = (seed >> 17); // 15-bit value [0, 32767]

        seed ^= seed << 13;
        seed ^= seed >> 17;
        seed ^= seed << 5;
        uint32_t y = (seed >> 17);

        if (x * x + y * y <= R2)
            hits++;
    }

    task->hits = hits;
    task->ts_finish = get_time_ms();
    atomic_int_set(&task->job_status, TASK_STATUS_FINISHED);
    return 0;
}

// ─── Per-worker init ─────────────────────────────────────────────────

static int init_worker(init_pool_worker_t* init_data, pool_worker_t* worker)
{
    (void)init_data; // unused
    worker->rand_seed = (unsigned int)time(NULL)
                      ^ (unsigned int)(uintptr_t)worker;
    if (worker->rand_seed == 0) worker->rand_seed = 0xDEADBEEF;
    return 0;
}

// ─── Main ────────────────────────────────────────────────────────────

int main(void)
{
    enable_vt_mode();

    int num_threads = get_num_cpus(THREAD_MULTIPLIER);

    if (g_vt_enabled)
        printf("\n\033[1;36m=== Monte Carlo Pi Estimation (Thread Pool Demo) ===\033[0m\n");
    else
        printf("\n=== Monte Carlo Pi Estimation (Thread Pool Demo) ===\n");
    printf("  Threads  : %d (auto-detected)\n", num_threads);
    printf("  Tasks    : %d\n", NUM_TASKS);
    printf("  Samples  : %llu per task  (%llu M total)\n\n",
           (unsigned long long)SAMPLES_PER_TASK,
           (unsigned long long)(NUM_TASKS * SAMPLES_PER_TASK / 1000000ULL));

    init_pool_worker_t init = {
        .init_func    = init_worker,
        .worker_func  = monte_carlo_worker,
        .destroy_func = NULL,
    };

    uint32_t t0 = get_time_ms();

    pool_t* pool = CreatePool(num_threads, &init);
    if (!pool)
    {
        fprintf(stderr, "ERROR: Failed to create thread pool\n");
        return 1;
    }

    task_t* tasks[NUM_TASKS];

    for (int i = 0; i < NUM_TASKS; i++)
    {
        tasks[i] = CreateTask();
        if (!tasks[i])
        {
            fprintf(stderr, "ERROR: Failed to create task %d\n", i);
            for (int j = 0; j < i; j++)
                DestroyTask(tasks[j]);
            DestroyPool(pool);
            return 1;
        }
        tasks[i]->task_id     = i;
        tasks[i]->num_samples = SAMPLES_PER_TASK;
        tasks[i]->hits        = 0;

        if (AddTaskToPool(pool, tasks[i]) != 0)
        {
            fprintf(stderr, "ERROR: Failed to add task %d to pool\n pool status: %d\n", i, atomic_int_get(&pool->status));
            for (int j = i; j >= 0; j--)
                DestroyTask(tasks[j]);
            DestroyPool(pool);
            return 1;
        }
    }

    printf("  All %d tasks submitted - computing...\n", NUM_TASKS);

    uint64_t total_hits    = 0;
    uint64_t total_samples = 0;

    int collected[NUM_TASKS];
    memset(collected, 0, sizeof(collected));
    int done = 0;

    while (done < NUM_TASKS)
    {
        for (int i = 0; i < NUM_TASKS; i++)
        {
            if (collected[i])
                continue;

            int status = atomic_int_get(&tasks[i]->job_status);
            if (status == TASK_STATUS_FINISHED)
            {
                total_hits    += tasks[i]->hits;
                total_samples += tasks[i]->num_samples;
                collected[i] = 1;
                done++;
            }
            else if (status == TASK_STATUS_ERROR || status == TASK_STATUS_STOP)
            {
                fprintf(stderr, "ERROR: Task %d failed (status=%d)\n", i, status);
                collected[i] = 1;
                done++;
            }
        }

        if (done < NUM_TASKS)
            sleep_ms(PROGRESS_POLL_SLEEP_MS);
    }

    printf("\n");

    uint32_t elapsed = get_time_ms() - t0;

    double pi_final = 4.0 * (double)total_hits / (double)total_samples;
    double error    = fabs(pi_final - M_PI);

    if (g_vt_enabled)
        printf("  \033[1;32mFinal estimate :\033[0m  %.9f\n",  pi_final);
    else
        printf("  Final estimate :  %.9f\n",  pi_final);
    printf("  Real Pi        :  %.9f\n",  M_PI);
    printf("  Absolute error :  %.3e  (%.5f%%)\n", error, 100.0 * error / M_PI);
    printf("  Wall time      :  %u ms\n\n", elapsed);

    uint32_t sum_handoff = 0, max_handoff = 0;   // submit → queued
    uint32_t sum_queue   = 0, max_queue   = 0;   // queued → dispatch
    uint32_t sum_pickup  = 0, max_pickup  = 0;   // dispatch → start
    uint32_t sum_exec    = 0, max_exec    = 0;   // start → finish
    uint32_t sum_total   = 0, max_total   = 0;   // submit → finish
    uint32_t first_submit = UINT32_MAX, last_submit = 0;
    uint32_t first_finish = UINT32_MAX, last_finish = 0;

    for (int i = 0; i < NUM_TASKS; i++)
    {
        uint32_t handoff = tasks[i]->ts_queued  - tasks[i]->ts_submit;
        uint32_t queue   = tasks[i]->ts_dispatch - tasks[i]->ts_queued;
        uint32_t pickup  = tasks[i]->ts_start   - tasks[i]->ts_dispatch;
        uint32_t exec    = tasks[i]->ts_finish  - tasks[i]->ts_start;
        uint32_t total   = tasks[i]->ts_finish  - tasks[i]->ts_submit;

        sum_handoff += handoff; if (handoff > max_handoff) max_handoff = handoff;
        sum_queue   += queue;   if (queue   > max_queue)   max_queue   = queue;
        sum_pickup  += pickup;  if (pickup  > max_pickup)  max_pickup  = pickup;
        sum_exec    += exec;    if (exec    > max_exec)    max_exec    = exec;
        sum_total   += total;   if (total   > max_total)   max_total   = total;

        if (tasks[i]->ts_submit < first_submit) first_submit = tasks[i]->ts_submit;
        if (tasks[i]->ts_submit > last_submit)  last_submit  = tasks[i]->ts_submit;
        if (tasks[i]->ts_finish < first_finish) first_finish = tasks[i]->ts_finish;
        if (tasks[i]->ts_finish > last_finish)  last_finish  = tasks[i]->ts_finish;
    }

    printf("  -- Pool timing stats (%d tasks) --\n", NUM_TASKS);
    printf("                          avg       max\n");
    printf("  Submit->Queued  :   %6u ms  %6u ms   (handoff to supervisor)\n",
           sum_handoff / NUM_TASKS, max_handoff);
    printf("  Queued->Dispatch:   %6u ms  %6u ms   (wait in queue)\n",
           sum_queue / NUM_TASKS, max_queue);
    printf("  Dispatch->Start :   %6u ms  %6u ms   (worker pickup latency)\n",
           sum_pickup / NUM_TASKS, max_pickup);
    printf("  Start->Finish   :   %6u ms  %6u ms   (worker_func execution)\n",
           sum_exec / NUM_TASKS, max_exec);
    printf("  Submit->Finish  :   %6u ms  %6u ms   (total task lifetime)\n",
           sum_total / NUM_TASKS, max_total);
    printf("  Submit window  :   %u ms  (first..last AddTaskToPool)\n",
           last_submit - first_submit);
    printf("  Compute window :   %u ms  (first start..last finish)\n\n",
           last_finish - first_finish);

    for (int i = 0; i < NUM_TASKS; i++)
        DestroyTask(tasks[i]);

    DestroyPool(pool);

    return 0;
}
