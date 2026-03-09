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

// Pull in the thread pool implementation (all functions are static inline)
#include "thread_pool.c"

#include <stdio.h>
#include <stdlib.h>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#include <math.h>
#include <time.h>

#ifdef _WIN32
static inline int rand_r(unsigned int *seed) {
    *seed = *seed * 1103515245u + 12345u;
    return (int)((*seed >> 16) & 0x7FFF);
}
#endif

// ─── Configuration ────────────────────────────────────────────────────────────

#define NUM_THREADS       20       // parallel worker threads
#define NUM_TASKS         80      // total work units
#define SAMPLES_PER_TASK  3000000ULL  // random points per task

// ─── Worker function ─────────────────────────────────────────────────────────
//
// Called by the thread pool for every dispatched task.
// Uses rand_r() with a per-worker seed for lock-free random number generation.
// MUST set job_status = TASK_STATUS_FINISHED before returning 0.

static int monte_carlo_worker(pool_worker_t* worker)
{
    task_t* task = worker->task;

    // Mix task id into the seed so each task explores a different sequence
    unsigned int seed = worker->rand_seed ^ (unsigned int)task->task_id;

    uint64_t hits = 0;
    for (uint64_t i = 0; i < task->num_samples; i++)
    {
        double x = (double)rand_r(&seed) / RAND_MAX;
        double y = (double)rand_r(&seed) / RAND_MAX;
        if (x * x + y * y <= 1.0)
            hits++;
    }

    task->hits = hits;
    atomic_int_set(&task->job_status, TASK_STATUS_FINISHED);
    return 0;
}

// ─── Per-worker init ──────────────────────────────────────────────────────────
//
// Gives every worker thread a unique random seed before any tasks run.

static int init_worker(init_pool_worker_t* init_data, pool_worker_t* worker)
{
    (void)init_data; // unused
    worker->rand_seed = (unsigned int)time(NULL)
                      ^ (unsigned int)(uintptr_t)worker;
    return 0;
}

// ─── Pretty progress bar ─────────────────────────────────────────────────────

#define BAR_WIDTH 32

static void print_progress(int done, int total, double pi, uint64_t samples)
{
    int filled = (done * BAR_WIDTH) / total;

    printf("\r  [");
    for (int i = 0; i < BAR_WIDTH; i++)
        putchar(i < filled ? '#' : '.');
    printf("] %2d/%-2d  ~Pi = \033[1;33m%.7f\033[0m  (%llu M samples)",
           done, total, pi,
           (unsigned long long)(samples / 1000000ULL));
    fflush(stdout);
}

// ─── Main ─────────────────────────────────────────────────────────────────────

int main(void)
{
    printf("\n\033[1;36m=== Monte Carlo Pi Estimation (Thread Pool Demo) ===\033[0m\n");
    printf("  Threads  : %d\n", NUM_THREADS);
    printf("  Tasks    : %d\n", NUM_TASKS);
    printf("  Samples  : %llu per task  (%llu M total)\n\n",
           (unsigned long long)SAMPLES_PER_TASK,
           (unsigned long long)(NUM_TASKS * SAMPLES_PER_TASK / 1000000ULL));

    // ── Build pool ──────────────────────────────────────────────────────────
    init_pool_worker_t init = {
        .init_func    = init_worker,
        .worker_func  = monte_carlo_worker,
        .destroy_func = NULL,
    };

    uint32_t t0 = get_time_ms();

    pool_t* pool = CreatePool(NUM_THREADS, &init);
    if (!pool)
    {
        fprintf(stderr, "ERROR: Failed to create thread pool\n");
        return 1;
    }

    // ── Create & submit all tasks ────────────────────────────────────────────
    task_t* tasks[NUM_TASKS];

    for (int i = 0; i < NUM_TASKS; i++)
    {
        tasks[i] = CreateTask();
        if (!tasks[i])
        {
            fprintf(stderr, "ERROR: Failed to create task %d\n", i);
            DestroyPool(pool);
            return 1;
        }
        tasks[i]->task_id     = i;
        tasks[i]->num_samples = SAMPLES_PER_TASK;
        tasks[i]->hits        = 0;

        if (AddTaskToPool(pool, tasks[i]) != 0)
        {
            fprintf(stderr, "ERROR: Failed to add task %d to pool\n pool status: %d\npool add free: %d\n ", i, atomic_int_get(&pool->status), atomic_int_get(&pool->free_to_add_task));
            DestroyPool(pool);
            return 1;
        }
    }

    printf("  All %d tasks submitted — computing", NUM_TASKS);
    fflush(stdout);

    // ── Collect results with live progress bar ────────────────────────────────
    uint64_t total_hits    = 0;
    uint64_t total_samples = 0;

    for (int i = 0; i < NUM_TASKS; i++)
    {
        if (WaitForFinishTask(tasks[i]) != 0)
        {
            fprintf(stderr, "\nERROR: Task %d timed out\n", i);
            continue;
        }

        total_hits    += tasks[i]->hits;
        total_samples += tasks[i]->num_samples;

        double pi_now = 4.0 * (double)total_hits / (double)total_samples;
        print_progress(i + 1, NUM_TASKS, pi_now, total_samples);
    }

    printf("\n\n");

    uint32_t elapsed = get_time_ms() - t0;

    // ── Final result ─────────────────────────────────────────────────────────
    double pi_final = 4.0 * (double)total_hits / (double)total_samples;
    double error    = fabs(pi_final - M_PI);

    printf("  \033[1;32mFinal estimate :\033[0m  %.9f\n",  pi_final);
    printf("  Real Pi        :  %.9f\n",  M_PI);
    printf("  Absolute error :  %.3e  (%.5f%%)\n", error, 100.0 * error / M_PI);
    printf("  Wall time      :  %u ms\n\n", elapsed);

    // ── Cleanup ──────────────────────────────────────────────────────────────
    for (int i = 0; i < NUM_TASKS; i++)
        DestroyTask(tasks[i]);

    DestroyPool(pool);

    return 0;
}
