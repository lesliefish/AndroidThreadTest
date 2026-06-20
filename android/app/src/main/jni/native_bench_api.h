// public API for the benchmark experiments
// included by both native_bench.c (executable) and jni_bridge.c (.so)

#ifndef NATIVE_BENCH_API_H
#define NATIVE_BENCH_API_H

// ─── Data structures ──────────────────────────────────────────────

typedef struct {
    int lo, hi;
    int* result;
} prime_task_t;

typedef struct {
    double median_ms, min_ms, max_ms, avg_ms;
    double p95_ms, p99_ms;
    double throughput_ops, speedup;
} bench_stats_t;

// ─── Thread operations (implemented in native_bench.c) ────────────

void*  android_thread_create(void* (*func)(void*), void* arg);
void   android_thread_join(void* handle);
void  android_thread_bind_cpu(int core_id);
int   android_get_core_count(void);
int*  android_get_cluster_map(int* out_count);

// ─── Workloads ────────────────────────────────────────────────────

void* prime_worker(void* arg);

// ─── Stats ────────────────────────────────────────────────────────

bench_stats_t compute_stats(double* timings, int num_runs,
                            double total_ops, double baseline_ms);

// ─── Experiments (public so JNI can call them) ────────────────────

void run_prime_experiment(const char* platform, int upper_bound, int runs);
void run_big_little_test(const char* platform, int upper_bound, int runs);
void run_hybrid_test(const char* platform, int upper_bound, int runs);

#endif // NATIVE_BENCH_API_H
