// Android native benchmark runner
// Runs all 3 experiments using pthreads + sched_setaffinity
// Output: CSV to stdout, parseable by analysis/plot_comparison.py

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <pthread.h>
#include <sched.h>
#include <unistd.h>
#include <sys/sysinfo.h>
#include <sys/time.h>
#include <errno.h>

// ─── Thread operations ────────────────────────────────────────────

struct thread_handle {
    pthread_t tid;
};

static void* pthread_wrapper(void* arg) {
    void* (*func)(void*) = (void*(*)(void*))(((void**)arg)[0]);
    void* user_arg = ((void**)arg)[1];
    free(arg);
    return func(user_arg);
}

static void* android_thread_create(void* (*func)(void*), void* arg) {
    void** wrapped = (void**)malloc(2 * sizeof(void*));
    wrapped[0] = (void*)func;
    wrapped[1] = arg;
    pthread_t tid;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 256 * 1024);
    if (pthread_create(&tid, &attr, pthread_wrapper, wrapped) != 0) {
        free(wrapped);
        return NULL;
    }
    pthread_attr_destroy(&attr);
    struct thread_handle* h = (struct thread_handle*)malloc(sizeof(*h));
    h->tid = tid;
    return (void*)h;
}

static void android_thread_join(void* handle) {
    if (!handle) return;
    struct thread_handle* h = (struct thread_handle*)handle;
    pthread_join(h->tid, NULL);
    free(h);
}

static void android_thread_bind_cpu(int core_id) {
    cpu_set_t mask;
    CPU_ZERO(&mask);
    CPU_SET(core_id, &mask);
    if (sched_setaffinity(0, sizeof(mask), &mask) != 0) {
        fprintf(stderr, "Warning: sched_setaffinity(cpu%d) failed: %s\n",
                core_id, strerror(errno));
    }
}

static int android_get_core_count(void) {
    return (int)sysconf(_SC_NPROCESSORS_ONLN);
}

static int* android_get_cluster_map(int* out_count) {
    int count = android_get_core_count();
    int* clusters = (int*)malloc(count * sizeof(int));
    char path[256];
    for (int i = 0; i < count; ++i) {
        snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/topology/cluster_id", i);
        FILE* f = fopen(path, "r");
        if (f) {
            int cid;
            if (fscanf(f, "%d", &cid) == 1) {
                clusters[i] = cid;
            } else {
                clusters[i] = 0;
            }
            fclose(f);
        } else {
            clusters[i] = (i < count / 2) ? 0 : 1;
        }
    }
    *out_count = count;
    return clusters;
}

// ─── Timing ───────────────────────────────────────────────────────

static double now_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec * 1000.0 + tv.tv_usec / 1000.0;
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

static void* prime_worker(void* arg) {
    prime_task_t* task = (prime_task_t*)arg;
    int count = 0;
    for (int i = task->lo; i <= task->hi; ++i) {
        if (is_prime(i)) ++count;
    }
    *(task->result) = count;
    return NULL;
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
    // Insertion sort (small N)
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
        handles[t] = android_thread_create(prime_worker, &ctx->tasks[t]);
    }
    for (int t = 0; t < ctx->num_threads; ++t) {
        android_thread_join(handles[t]);
    }
    free(handles);
}

static void run_prime_experiment(const char* platform, int upper_bound, int runs) {
    int core_count = android_get_core_count();
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

            // Free per-thread results
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

// ─── Experiment 2: Big.LITTLE separation ──────────────────────────

static void run_big_little_test(const char* platform, int upper_bound, int runs) {
    int core_count;
    int* clusters = android_get_cluster_map(&core_count);

    // Count clusters
    int max_cluster = 0;
    for (int i = 0; i < core_count; ++i) {
        if (clusters[i] > max_cluster) max_cluster = clusters[i];
    }
    int num_clusters = max_cluster + 1;

    printf("\n=== Big.LITTLE Test (%s, %d cores) ===\n", platform, core_count);
    printf("cluster,core_count,median_ms,min_ms,max_ms,throughput_ops\n");

    for (int cid = 0; cid < num_clusters; ++cid) {
        // Collect cores in this cluster
        int* ccores = (int*)malloc(core_count * sizeof(int));
        int cc_count = 0;
        for (int i = 0; i < core_count; ++i) {
            if (clusters[i] == cid) ccores[cc_count++] = i;
        }

        // Run prime counting bound to these cores
        double* timings = (double*)malloc(runs * sizeof(double));
        for (int run = 0; run < runs; ++run) {
            // Create one thread per core in this cluster, each bound
            void** handles = (void**)malloc(cc_count * sizeof(void*));
            prime_task_t* tasks = (prime_task_t*)malloc(cc_count * sizeof(prime_task_t));

            for (int t = 0; t < cc_count; ++t) {
                android_thread_bind_cpu(ccores[t]);
                tasks[t].lo = 0;
                tasks[t].hi = upper_bound - 1;
                tasks[t].result = (int*)malloc(sizeof(int));
                handles[t] = android_thread_create(prime_worker, &tasks[t]);
            }

            // Unbind to default affinity for timing
            cpu_set_t all_cpus;
            CPU_ZERO(&all_cpus);
            for (int i = 0; i < core_count; ++i) CPU_SET(i, &all_cpus);
            sched_setaffinity(0, sizeof(all_cpus), &all_cpus);

            double start = now_ms();
            for (int t = 0; t < cc_count; ++t) {
                android_thread_join(handles[t]);
                free(tasks[t].result);
            }
            timings[run] = now_ms() - start;

            free(tasks);
            free(handles);
        }

        // Compute stats
        double sum = 0;
        for (int i = 0; i < runs; ++i) sum += timings[i];
        double avg = sum / runs;
        // Simple min/max
        double mn = timings[0], mx = timings[0];
        for (int i = 1; i < runs; ++i) {
            if (timings[i] < mn) mn = timings[i];
            if (timings[i] > mx) mx = timings[i];
        }
        // Sort for median
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

        const char* cname = (cid == 0) ? "little(E-core)" : "big(P-core)";
        printf("%s,%d,%.3f,%.3f,%.3f,%.0f\n", cname, cc_count, median, mn, mx, tp);

        printf("  %-15s (%d cores): median=%.3fms  min=%.3fms  max=%.3fms  throughput=%.0f ops/s\n",
               cname, cc_count, median, mn, mx, tp);

        free(sorted);
        free(timings);
        free(ccores);
    }

    free(clusters);
}

// ─── Experiment 3: Hybrid scheduling ──────────────────────────────

static void run_hybrid_test(const char* platform, int upper_bound, int runs) {
    int core_count;
    int* clusters = android_get_cluster_map(&core_count);

    int max_cluster = 0;
    for (int i = 0; i < core_count; ++i)
        if (clusters[i] > max_cluster) max_cluster = clusters[i];
    int num_clusters = max_cluster + 1;

    // Count cores per cluster
    int* cluster_sizes = (int*)calloc(num_clusters, sizeof(int));
    for (int i = 0; i < core_count; ++i) cluster_sizes[clusters[i]]++;

    printf("\n=== Hybrid Scheduling Test (%s, %d cores) ===\n", platform, core_count);
    printf("scenario,thread_count,median_ms,min_ms,max_ms,throughput_ops\n");

    // Collect core indices per cluster
    int** cluster_cores = (int**)malloc(num_clusters * sizeof(int*));
    for (int c = 0; c < num_clusters; ++c) {
        cluster_cores[c] = (int*)malloc(core_count * sizeof(int));
        int cnt = 0;
        for (int i = 0; i < core_count; ++i) {
            if (clusters[i] == c) cluster_cores[c][cnt++] = i;
        }
        cluster_cores[c] = (int*)realloc(cluster_cores[c], cnt * sizeof(int));
    }

    // Scenario A: All threads on big cores only
    if (cluster_sizes[num_clusters - 1] > 0) {
        int big_n = cluster_sizes[num_clusters - 1];
        double* timings = (double*)malloc(runs * sizeof(double));
        for (int run = 0; run < runs; ++run) {
            void** handles = (void**)malloc(big_n * sizeof(void*));
            prime_task_t* tasks = (prime_task_t*)malloc(big_n * sizeof(prime_task_t));
            int chunk = upper_bound / big_n;
            for (int t = 0; t < big_n; ++t) {
                tasks[t].lo = t * chunk;
                tasks[t].hi = (t + 1) * chunk - 1;
                if (t == big_n - 1) tasks[t].hi = upper_bound - 1;
                tasks[t].result = (int*)malloc(sizeof(int));
                handles[t] = android_thread_create(prime_worker, &tasks[t]);
            }
            double start = now_ms();
            for (int t = 0; t < big_n; ++t) {
                android_thread_join(handles[t]);
                free(tasks[t].result);
            }
            timings[run] = now_ms() - start;
            free(tasks);
            free(handles);
        }
        double sum = 0;
        for (int i = 0; i < runs; ++i) sum += timings[i];
        double avg = sum / runs;
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
        double tp = (double)upper_bound / (median / 1000.0);
        printf("ALL_BIG,%d,%.3f,%.3f,%.3f,%.0f\n", big_n, median, mn, mx, tp);
        printf("  ALL_BIG:   %d threads, median=%.3fms, throughput=%.0f ops/s\n", big_n, median, tp);
        free(sorted);
        free(timings);
    }

    // Scenario B: All threads on little cores only
    if (cluster_sizes[0] > 0) {
        int lit_n = cluster_sizes[0];
        double* timings = (double*)malloc(runs * sizeof(double));
        for (int run = 0; run < runs; ++run) {
            void** handles = (void**)malloc(lit_n * sizeof(void*));
            prime_task_t* tasks = (prime_task_t*)malloc(lit_n * sizeof(prime_task_t));
            int chunk = upper_bound / lit_n;
            for (int t = 0; t < lit_n; ++t) {
                tasks[t].lo = t * chunk;
                tasks[t].hi = (t + 1) * chunk - 1;
                if (t == lit_n - 1) tasks[t].hi = upper_bound - 1;
                tasks[t].result = (int*)malloc(sizeof(int));
                handles[t] = android_thread_create(prime_worker, &tasks[t]);
            }
            double start = now_ms();
            for (int t = 0; t < lit_n; ++t) {
                android_thread_join(handles[t]);
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
        double tp = (double)upper_bound / (median / 1000.0);
        printf("ALL_LITTLE,%d,%.3f,%.3f,%.3f,%.0f\n", lit_n, median, mn, mx, tp);
        printf("  ALL_LITTLE: %d threads, median=%.3fms, throughput=%.0f ops/s\n", lit_n, median, tp);
        free(sorted);
        free(timings);
    }

    // Scenario C: Hybrid — compute on big, light work on little
    {
        int big_n = cluster_sizes[num_clusters - 1];
        int lit_n = cluster_sizes[0];
        int total_n = big_n + lit_n;
        if (big_n > 0 && lit_n > 0) {
            double* timings = (double*)malloc(runs * sizeof(double));
            for (int run = 0; run < runs; ++run) {
                void** handles = (void**)malloc(total_n * sizeof(void*));
                prime_task_t* tasks = (prime_task_t*)malloc(total_n * sizeof(prime_task_t));

                // Big cores: full workload
                int chunk_big = upper_bound / big_n;
                for (int t = 0; t < big_n; ++t) {
                    tasks[t].lo = t * chunk_big;
                    tasks[t].hi = (t + 1) * chunk_big - 1;
                    if (t == big_n - 1) tasks[t].hi = upper_bound - 1;
                    tasks[t].result = (int*)malloc(sizeof(int));
                    handles[t] = android_thread_create(prime_worker, &tasks[t]);
                }
                // Little cores: lightweight work (just count even numbers)
                for (int t = 0; t < lit_n; ++t) {
                    tasks[t].lo = 0;
                    tasks[t].hi = 10000;  // tiny workload
                    tasks[t].result = (int*)malloc(sizeof(int));
                    handles[t] = android_thread_create(prime_worker, &tasks[t]);
                }

                double start = now_ms();
                for (int t = 0; t < total_n; ++t) {
                    android_thread_join(handles[t]);
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
            printf("HYBRID,%d,%.3f,%.3f,%.3f,%.0f\n", total_n, median, mn, mx, 0.0);
            printf("  HYBRID:    %d threads (%d big + %d little), median=%.3fms\n",
                   total_n, big_n, lit_n, median);
            free(sorted);
            free(timings);
        }
    }

    // Cleanup
    for (int c = 0; c < num_clusters; ++c) free(cluster_cores[c]);
    free(cluster_cores);
    free(cluster_sizes);
    free(clusters);
}

// ─── Main ──────────────────────────────────────────────────────────

int main(int argc, char* argv[]) {
    const char* platform = "android";
    int upper_bound = 1000000;  // count primes up to N
    int runs = 10;

    if (argc > 1) platform = argv[1];
    if (argc > 2) upper_bound = atoi(argv[2]);
    if (argc > 3) runs = atoi(argv[3]);

    printf("Platform: %s\n", platform);
    printf("Prime upper bound: %d\n", upper_bound);
    printf("Runs per config: %d\n", runs);
    printf("Core count: %d\n\n", android_get_core_count());

    // Experiment 1: Single vs Multi-thread
    printf("=== Experiment 1: Prime Counting (Single vs Multi-thread) ===\n");
    run_prime_experiment(platform, upper_bound, runs);

    // Experiment 2: Big.LITTLE separation
    printf("\n=== Experiment 2: Big.LITTLE Separation ===\n");
    run_big_little_test(platform, upper_bound, runs);

    // Experiment 3: Hybrid scheduling
    printf("\n=== Experiment 3: Hybrid Scheduling ===\n");
    run_hybrid_test(platform, upper_bound, runs);

    return 0;
}
