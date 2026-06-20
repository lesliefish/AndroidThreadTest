// Windows native benchmark runner
// Same algorithms as Android, using Win32 APIs
// Output: CSV to stdout, parseable by analysis/plot_comparison.py

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <windows.h>
#include <process.h>

// ─── Thread operations ────────────────────────────────────────────

typedef struct {
    HANDLE h;
} win_thread_handle;

typedef struct {
    void (*func)(void*);
    void* arg;
} thread_wrap_t;

static unsigned __stdcall thread_runner(void* arg) {
    thread_wrap_t* w = (thread_wrap_t*)arg;
    w->func(w->arg);
    free(w);
    return 0;
}

static void* win_thread_create(void (*func)(void*), void* arg) {
    thread_wrap_t* w = (thread_wrap_t*)malloc(sizeof(*w));
    w->func = func;
    w->arg = arg;
    HANDLE h = (HANDLE)_beginthreadex(NULL, 0, thread_runner, w, 0, NULL);
    if (!h) { free(w); return NULL; }

    win_thread_handle* wh = (win_thread_handle*)malloc(sizeof(*wh));
    wh->h = h;
    return (void*)wh;
}

static void win_thread_join(void* handle) {
    if (!handle) return;
    win_thread_handle* wh = (win_thread_handle*)handle;
    WaitForSingleObject(wh->h, INFINITE);
    CloseHandle(wh->h);
    free(wh);
}

static void win_thread_bind_cpu(int core_id) {
    DWORD_PTR mask = (DWORD_PTR)1ULL << core_id;
    SetThreadAffinityMask(GetCurrentThread(), mask);
}

static int win_get_core_count(void) {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return (int)si.dwNumberOfProcessors;
}

// Windows doesn't expose big.LITTLE topology directly
// Fallback: first half = cluster 0, second half = cluster 1
static int* win_get_cluster_map(int* out_count) {
    int count = win_get_core_count();
    int* clusters = (int*)calloc(count, sizeof(int));
    for (int i = 0; i < count; ++i) {
        clusters[i] = (i < count / 2) ? 0 : 1;
    }
    *out_count = count;
    return clusters;
}

// ─── Timing ───────────────────────────────────────────────────────

static double now_ms(void) {
    static LARGE_INTEGER freq = {0};
    if (freq.QuadPart == 0) QueryPerformanceFrequency(&freq);
    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);
    return (double)counter.QuadPart / (double)freq.QuadPart * 1000.0;
}

// ─── Workload: Prime counting ─────────────────────────────────────

static int is_prime(int n) {
    if (n < 2) return 0;
    if (n < 4) return 1;
    if (n % 2 == 0 || n % 3 == 0) return 0;
    for (int i = 5; (long long)i * i <= n; i += 6) {
        if (n % i == 0 || n % (i + 2) == 0) return 0;
    }
    return 1;
}

typedef struct {
    int lo, hi;
    int* result;
} prime_task_t;

static void prime_worker(void* arg) {
    prime_task_t* task = (prime_task_t*)arg;
    int count = 0;
    for (int i = task->lo; i <= task->hi; ++i) {
        if (is_prime(i)) ++count;
    }
    *(task->result) = count;
}

// ─── Stats computation ────────────────────────────────────────────

typedef struct {
    double median_ms, min_ms, max_ms, avg_ms;
    double p95_ms, p99_ms;
    double throughput_ops, speedup;
} bench_stats_t;

static bench_stats_t compute_stats(double* timings, int num_runs,
                                    double total_ops, double baseline_ms) {
    bench_stats_t s = {0};
    if (num_runs == 0) return s;

    double* sorted = (double*)malloc(num_runs * sizeof(double));
    memcpy(sorted, timings, num_runs * sizeof(double));
    for (int i = 1; i < num_runs; ++i) {
        double key = sorted[i];
        int j = i - 1;
        while (j >= 0 && sorted[j] > key) {
            sorted[j + 1] = sorted[j];
            --j;
        }
        sorted[j + 1] = key;
    }

    double sum = 0;
    for (int i = 0; i < num_runs; ++i) sum += sorted[i];
    s.avg_ms = sum / num_runs;
    s.median_ms = sorted[num_runs / 2];
    s.min_ms = sorted[0];
    s.max_ms = sorted[num_runs - 1];
    s.p95_ms = sorted[(int)(num_runs * 0.95)];
    s.p99_ms = sorted[(int)(num_runs * 0.99)];
    s.throughput_ops = total_ops / (s.median_ms / 1000.0);
    s.speedup = (baseline_ms > 0) ? (baseline_ms / s.median_ms) : 1.0;

    free(sorted);
    return s;
}

// ─── Experiment 1: Prime counting, varying thread count ───────────

typedef struct {
    int num_threads;
    int upper_bound;
    prime_task_t* tasks;
} prime_run_ctx_t;

static void prime_run_worker(void* arg) {
    prime_run_ctx_t* ctx = (prime_run_ctx_t*)arg;
    void** handles = (void**)malloc(ctx->num_threads * sizeof(void*));
    for (int t = 0; t < ctx->num_threads; ++t) {
        handles[t] = win_thread_create(prime_worker, &ctx->tasks[t]);
    }
    for (int t = 0; t < ctx->num_threads; ++t) {
        win_thread_join(handles[t]);
    }
    free(handles);
}

static void run_prime_experiment(const char* platform, int upper_bound, int runs) {
    int core_count = win_get_core_count();
    int thread_counts[] = {1, 2, 4, 6, 8, 12, 16};
    int num_configs = 0;
    for (int i = 0; i < (int)(sizeof(thread_counts)/sizeof(thread_counts[0])); ++i) {
        if (thread_counts[i] <= core_count) num_configs = i + 1;
        else break;
    }

    printf("experiment,platform,num_threads,median_ms,min_ms,max_ms,avg_ms,p95_ms,p99_ms,throughput_ops,speedup\n");

    // Baseline: 1 thread
    prime_task_t baseline_task;
    baseline_task.lo = 0;
    baseline_task.hi = upper_bound - 1;
    int baseline_result = 0;
    baseline_task.result = &baseline_result;

    double baseline_ms = 0;
    for (int run = 0; run < runs; ++run) {
        baseline_task.result = &baseline_result;
        double start = now_ms();
        prime_worker(&baseline_task);
        baseline_ms = now_ms() - start;
    }

    // Run with multiple threads
    for (int ci = 0; ci < num_configs; ++ci) {
        int nt = thread_counts[ci];
        prime_task_t* tasks = (prime_task_t*)malloc(nt * sizeof(prime_task_t));
        double* timings = (double*)malloc(runs * sizeof(double));

        for (int run = 0; run < runs; ++run) {
            for (int t = 0; t < nt; ++t) {
                tasks[t].lo = t * upper_bound / nt;
                tasks[t].hi = (t + 1) * upper_bound / nt - 1;
                if (t == nt - 1) tasks[t].hi = upper_bound - 1;
                tasks[t].result = (int*)malloc(sizeof(int));
            }

            prime_run_ctx_t ctx = {nt, upper_bound, tasks};
            timings[run] = now_ms();
            prime_run_worker(&ctx);
            timings[run] = now_ms() - timings[run];

            for (int t = 0; t < nt; ++t) free(tasks[t].result);
        }

        bench_stats_t s = compute_stats(timings, runs, upper_bound, baseline_ms);
        printf("prime_counting,%s,%d,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.0f,%.2f\n",
               platform, nt, s.median_ms, s.min_ms, s.max_ms, s.avg_ms,
               s.p95_ms, s.p99_ms, s.throughput_ops, s.speedup);

        free(timings);
        free(tasks);
    }
}

// ─── Experiment 2: Big.LITTLE simulation ──────────────────────────

static void run_big_little_test(const char* platform, int upper_bound, int runs) {
    int core_count;
    int* clusters = win_get_cluster_map(&core_count);

    int max_cluster = 0;
    for (int i = 0; i < core_count; ++i)
        if (clusters[i] > max_cluster) max_cluster = clusters[i];
    int num_clusters = max_cluster + 1;

    printf("\n=== Big.LITTLE Simulation (%s, %d cores) ===\n", platform, core_count);
    printf("(Windows does not expose big.LITTLE topology; using heuristic split)\n");
    printf("cluster,core_count,median_ms,min_ms,max_ms,throughput_ops\n");

    for (int cid = 0; cid < num_clusters; ++cid) {
        int* ccores = (int*)malloc(core_count * sizeof(int));
        int cc_count = 0;
        for (int i = 0; i < core_count; ++i) {
            if (clusters[i] == cid) ccores[cc_count++] = i;
        }

        double* timings = (double*)malloc(runs * sizeof(double));
        for (int run = 0; run < runs; ++run) {
            void** handles = (void**)malloc(cc_count * sizeof(void*));
            prime_task_t* tasks = (prime_task_t*)malloc(cc_count * sizeof(prime_task_t));
            int chunk = upper_bound / cc_count;
            for (int t = 0; t < cc_count; ++t) {
                tasks[t].lo = t * chunk;
                tasks[t].hi = (t + 1) * chunk - 1;
                if (t == cc_count - 1) tasks[t].hi = upper_bound - 1;
                tasks[t].result = (int*)malloc(sizeof(int));
                handles[t] = win_thread_create(prime_worker, &tasks[t]);
            }
            double start = now_ms();
            for (int t = 0; t < cc_count; ++t) {
                win_thread_join(handles[t]);
                free(tasks[t].result);
            }
            timings[run] = now_ms() - start;
            free(tasks);
            free(handles);
        }

        double mn = timings[0], mx = timings[0];
        double* sorted = (double*)malloc(runs * sizeof(double));
        memcpy(sorted, timings, runs * sizeof(double));
        for (int i = 1; i < runs; ++i) {
            double key = sorted[i];
            int j = i - 1;
            while (j >= 0 && sorted[j] > key) { sorted[j+1] = sorted[j]; --j; }
            sorted[j+1] = key;
        }
        double median = sorted[runs / 2];
        double tp = (double)upper_bound * cc_count / (median / 1000.0);

        const char* cname = (cid == 0) ? "cluster0" : "cluster1";
        printf("%s,%d,%.3f,%.3f,%.3f,%.0f\n", cname, cc_count, median, mn, mx, tp);
        printf("  %-10s (%d cores): median=%.3fms  throughput=%.0f ops/s\n",
               cname, cc_count, median, tp);

        free(sorted);
        free(timings);
        free(ccores);
    }

    free(clusters);
}

// ─── Experiment 3: Hybrid scheduling ──────────────────────────────

static void run_hybrid_test(const char* platform, int upper_bound, int runs) {
    int core_count = win_get_core_count();
    int half = core_count / 2;

    printf("\n=== Hybrid Scheduling Test (%s, %d cores) ===\n", platform, core_count);
    printf("scenario,thread_count,median_ms,min_ms,max_ms\n");

    // Scenario A: All on "big" (second half)
    if (half > 0) {
        double* timings = (double*)malloc(runs * sizeof(double));
        for (int run = 0; run < runs; ++run) {
            void** handles = (void**)malloc(half * sizeof(void*));
            prime_task_t* tasks = (prime_task_t*)malloc(half * sizeof(prime_task_t));
            int chunk = upper_bound / half;
            for (int t = 0; t < half; ++t) {
                tasks[t].lo = t * chunk;
                tasks[t].hi = (t + 1) * chunk - 1;
                if (t == half - 1) tasks[t].hi = upper_bound - 1;
                tasks[t].result = (int*)malloc(sizeof(int));
                handles[t] = win_thread_create(prime_worker, &tasks[t]);
            }
            double start = now_ms();
            for (int t = 0; t < half; ++t) {
                win_thread_join(handles[t]);
                free(tasks[t].result);
            }
            timings[run] = now_ms() - start;
            free(tasks);
            free(handles);
        }
        double mn = timings[0], mx = timings[0];
        double* sorted = (double*)malloc(runs * sizeof(double));
        memcpy(sorted, timings, runs * sizeof(double));
        for (int i = 1; i < runs; ++i) {
            double key = sorted[i];
            int j = i - 1;
            while (j >= 0 && sorted[j] > key) { sorted[j+1] = sorted[j]; --j; }
            sorted[j+1] = key;
        }
        double median = sorted[runs / 2];
        printf("ALL_BIG,%d,%.3f,%.3f,%.3f\n", half, median, mn, mx);
        printf("  ALL_BIG:   %d threads, median=%.3fms\n", half, median);
        free(sorted);
        free(timings);
    }

    // Scenario B: All on "little" (first half)
    if (half > 0) {
        double* timings = (double*)malloc(runs * sizeof(double));
        for (int run = 0; run < runs; ++run) {
            void** handles = (void**)malloc(half * sizeof(void*));
            prime_task_t* tasks = (prime_task_t*)malloc(half * sizeof(prime_task_t));
            int chunk = upper_bound / half;
            for (int t = 0; t < half; ++t) {
                tasks[t].lo = t * chunk;
                tasks[t].hi = (t + 1) * chunk - 1;
                if (t == half - 1) tasks[t].hi = upper_bound - 1;
                tasks[t].result = (int*)malloc(sizeof(int));
                handles[t] = win_thread_create(prime_worker, &tasks[t]);
            }
            double start = now_ms();
            for (int t = 0; t < half; ++t) {
                win_thread_join(handles[t]);
                free(tasks[t].result);
            }
            timings[run] = now_ms() - start;
            free(tasks);
            free(handles);
        }
        double mn = timings[0], mx = timings[0];
        double* sorted = (double*)malloc(runs * sizeof(double));
        memcpy(sorted, timings, runs * sizeof(double));
        for (int i = 1; i < runs; ++i) {
            double key = sorted[i];
            int j = i - 1;
            while (j >= 0 && sorted[j] > key) { sorted[j+1] = sorted[j]; --j; }
            sorted[j+1] = key;
        }
        double median = sorted[runs / 2];
        printf("ALL_LITTLE,%d,%.3f,%.3f,%.3f\n", half, median, mn, mx);
        printf("  ALL_LITTLE: %d threads, median=%.3fms\n", half, median);
        free(sorted);
        free(timings);
    }

    // Scenario C: Hybrid
    {
        int total_n = core_count;
        double* timings = (double*)malloc(runs * sizeof(double));
        for (int run = 0; run < runs; ++run) {
            void** handles = (void**)malloc(total_n * sizeof(void*));
            prime_task_t* tasks = (prime_task_t*)malloc(total_n * sizeof(prime_task_t));

            // First half: heavy workload
            int chunk_heavy = upper_bound / (total_n / 2);
            for (int t = 0; t < total_n / 2; ++t) {
                tasks[t].lo = t * chunk_heavy;
                tasks[t].hi = (t + 1) * chunk_heavy - 1;
                if (t == total_n/2 - 1) tasks[t].hi = upper_bound - 1;
                tasks[t].result = (int*)malloc(sizeof(int));
                handles[t] = win_thread_create(prime_worker, &tasks[t]);
            }
            // Second half: light workload
            for (int t = total_n / 2; t < total_n; ++t) {
                tasks[t].lo = 0;
                tasks[t].hi = 1000;  // tiny
                tasks[t].result = (int*)malloc(sizeof(int));
                handles[t] = win_thread_create(prime_worker, &tasks[t]);
            }

            double start = now_ms();
            for (int t = 0; t < total_n; ++t) {
                win_thread_join(handles[t]);
                free(tasks[t].result);
            }
            timings[run] = now_ms() - start;
            free(tasks);
            free(handles);
        }
        double mn = timings[0], mx = timings[0];
        double* sorted = (double*)malloc(runs * sizeof(double));
        memcpy(sorted, timings, runs * sizeof(double));
        for (int i = 1; i < runs; ++i) {
            double key = sorted[i];
            int j = i - 1;
            while (j >= 0 && sorted[j] > key) { sorted[j+1] = sorted[j]; --j; }
            sorted[j+1] = key;
        }
        double median = sorted[runs / 2];
        printf("HYBRID,%d,%.3f,%.3f,%.3f\n", total_n, median, mn, mx);
        printf("  HYBRID:    %d threads, median=%.3fms\n", total_n, median);
        free(sorted);
        free(timings);
    }
}

// ─── Main ──────────────────────────────────────────────────────────

int main(int argc, char* argv[]) {
    const char* platform = "windows";
    int upper_bound = 1000000;
    int runs = 10;

    if (argc > 1) platform = argv[1];
    if (argc > 2) upper_bound = atoi(argv[2]);
    if (argc > 3) runs = atoi(argv[3]);

    printf("Platform: %s\n", platform);
    printf("Prime upper bound: %d\n", upper_bound);
    printf("Runs per config: %d\n", runs);
    printf("Core count: %d\n\n", win_get_core_count());

    // Experiment 1: Single vs Multi-thread
    printf("=== Experiment 1: Prime Counting (Single vs Multi-thread) ===\n");
    run_prime_experiment(platform, upper_bound, runs);

    // Experiment 2: Big.LITTLE simulation
    printf("\n=== Experiment 2: Big.LITTLE Simulation ===\n");
    run_big_little_test(platform, upper_bound, runs);

    // Experiment 3: Hybrid scheduling
    printf("\n=== Experiment 3: Hybrid Scheduling ===\n");
    run_hybrid_test(platform, upper_bound, runs);

    return 0;
}
