// Windows 原生基准测试运行器
//
// 功能：
//   在与 Android 版本相同的三项实验中使用 Windows API
//   （CreateThread / SetThreadAffinityMask）执行，以对比不同操作系统内核调度器
//   （NT 调度器 vs Linux CFS）对同一 C 代码的影响。
//
// 与 Android 版本的主要区别：
//   - 线程创建：使用 _beginthreadex 而非 pthread_create
//   - 线程等待：使用 WaitForSingleObject 而非 pthread_join
//   - 核心亲和：使用 SetThreadAffinityMask 而非 sched_setaffinity
//   - 时间测量：使用 QueryPerformanceCounter 而非 gettimeofday
//   - 大小核：Windows 不暴露 big.LITTLE 拓扑，因此使用
//     启发式拆分（前一半核心 vs 后一半核心）
//
// 输出：
//   CSV 格式到 stdout，与 Android 版本完全一致，便于通过
//   analysis/plot_comparison.py 进行跨平台对比。

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <windows.h>
#include <process.h>

// ─── 线程操作 ─────────────────────────────────────────────────────
// 围绕 Windows 线程 API 的平台特定封装。
// 这些函数对应 Android 版本中的 android_thread_* 函数。

// win_thread_handle — Windows 线程句柄的透明封装。
// 对应 Android 端的 struct thread_handle。
typedef struct {
    HANDLE h;
} win_thread_handle;

// thread_wrap_t — _beginthreadex 的参数打包结构体。
//
// _beginthreadex 要求函数指针类型为 `unsigned (__stdcall*)(void*)`。
// 我们的工作函数类型为 `void (*)(void*)`，因此需要一个适配器。
// 此结构体保存函数指针和用户参数，thread_runner() 调用实际的工作函数
// 然后释放此结构体。
typedef struct {
    void (*func)(void*);
    void* arg;
} thread_wrap_t;

// thread_runner — _beginthreadex 的适配器函数。
//
// 将通用 void* 参数转回 thread_wrap_t*，调用实际的工作函数，
// 释放包装器，并返回 0（线程退出码）。
static unsigned __stdcall thread_runner(void* arg) {
    thread_wrap_t* w = (thread_wrap_t*)arg;
    w->func(w->arg);
    free(w);
    return 0;
}

// win_thread_create — Windows 平台的跨平台线程创建封装。
//
// 使用 _beginthreadex（_beginthread 的安全变体）创建线程。
// 返回一个供 win_thread_join 使用的透明句柄。
// 失败时返回 NULL。
//
// 为什么用 _beginthreadex 而非 CreateThread？
//   _beginthreadex 正确初始化了 CRT（C 运行时）的每线程状态
//   （errno、线程局部存储等）。CreateThread 跳过这一步，
//   当在线程边界之间混合使用 C 库调用时可能导致微妙崩溃。
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

// win_thread_join — 等待由 win_thread_create 创建的线程结束。
//
// 使用 INFINITE 超时的 WaitForSingleObject（阻塞直到线程退出）。
// 然后关闭 HANDLE 并释放包装结构体。
static void win_thread_join(void* handle) {
    if (!handle) return;
    win_thread_handle* wh = (win_thread_handle*)handle;
    WaitForSingleObject(wh->h, INFINITE);
    CloseHandle(wh->h);
    free(wh);
}

// win_thread_bind_cpu — 将调用线程绑定到指定 CPU 核心。
//
// 使用 SetThreadAffinityMask 将线程限制到单个核心。
// 注意：SetThreadAffinityMask 返回之前的亲和性掩码，此处忽略。
// 调用者应在完成后恢复之前的掩码。
static void win_thread_bind_cpu(int core_id) {
    DWORD_PTR mask = (DWORD_PTR)1ULL << core_id;
    SetThreadAffinityMask(GetCurrentThread(), mask);
}

// win_get_core_count — 返回逻辑处理器数量。
//
// 使用 GetSystemInfo 从操作系统查询处理器数量。
// 相当于 Linux 上的 sysconf(_SC_NPROCESSORS_ONLN)。
static int win_get_core_count(void) {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return (int)si.dwNumberOfProcessors;
}

// win_get_cluster_map — 检测 Windows 上的核心集群。
//
// Windows 不通过任何公共 API 暴露 big.LITTLE 拓扑。
// （不像 Linux 上可以直接读取
//  /sys/devices/system/cpu/cpu*/topology/cluster_id）
//
// 回退启发式：将核心对半分。
//   前一半  → 集群 0（视为"小核"）
//   后一半  → 集群 1（视为"大核"）
//
// 这在真正的 big.LITTLE 硬件上并不准确，但为基准测试代码
// 提供了一致的接口。在 x86 桌面端（所有核心通常相同）上，
// 这影响不大。
static int* win_get_cluster_map(int* out_count) {
    int count = win_get_core_count();
    int* clusters = (int*)calloc(count, sizeof(int));
    for (int i = 0; i < count; ++i) {
        clusters[i] = (i < count / 2) ? 0 : 1;
    }
    *out_count = count;
    return clusters;
}

// ─── 时间测量 ─────────────────────────────────────────────────────
// 使用 QueryPerformanceCounter 实现高精度时间测量。
// 在 Windows 上提供纳秒级精度，远高于 Android 端的 gettimeofday()。

static double now_ms(void) {
    // 懒初始化频率计数器（仅调用一次）。
    static LARGE_INTEGER freq = {0};
    if (freq.QuadPart == 0) QueryPerformanceFrequency(&freq);

    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);
    // 将滴答数转换为毫秒
    return (double)counter.QuadPart / (double)freq.QuadPart * 1000.0;
}

// ─── 工作负载：素数计数 ───────────────────────────────────────────
// 与 Android 版本完全相同 — 相同的算法，相同的复杂度。
// 这确保了公平比较：唯一变量是 OS 调度器。

// is_prime — 带 6k±1 优化的试除法。
// 最坏情况 O(sqrt(n))，确定性输出。
static int is_prime(int n) {
    if (n < 2) return 0;
    if (n < 4) return 1;
    if (n % 2 == 0 || n % 3 == 0) return 0;
    for (int i = 5; (long long)i * i <= n; i += 6) {
        if (n % i == 0 || n % (i + 2) == 0) return 0;
    }
    return 1;
}

// prime_task_t — 描述一个需要素数计数的范围 [lo, hi]。
typedef struct {
    int lo, hi;     // 包含边界的范围
    int* result;    // 输出指针（工作线程写入）
} prime_task_t;

// prime_worker — 统计 [lo, hi] 范围内的素数，写入结果。
// 与 Android 版本逻辑完全一致。
static void prime_worker(void* arg) {
    prime_task_t* task = (prime_task_t*)arg;
    int count = 0;
    for (int i = task->lo; i <= task->hi; ++i) {
        if (is_prime(i)) ++count;
    }
    *(task->result) = count;
}

// ─── 统计计算 ─────────────────────────────────────────────────────
// 与 Android 版本完全相同 — 相同的排序算法，相同的指标。

typedef struct {
    double median_ms, min_ms, max_ms, avg_ms;
    double p95_ms, p99_ms;
    double throughput_ops, speedup;
} bench_stats_t;

static bench_stats_t compute_stats(double* timings, int num_runs,
                                    double total_ops, double baseline_ms) {
    bench_stats_t s = {0};
    if (num_runs == 0) return s;

    // 复制并插入排序（对小 N 高效）
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

// ─── 实验 1：不同线程数下的素数计数 ─────────────────────────────────
// 与 Android 版本方法完全一致。
// 测试 1/2/4/6/8/12/16 线程（过滤到 <= core_count）。
// 计算相对于 1 线程基线的加速比。

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

    // 基线：1 线程
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

    // 多线程运行
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

// ─── 实验 2：大小核模拟 ────────────────────────────────────────────
//
// 注意：Windows 不暴露 big.LITTLE 拓扑。
// 本实验使用启发式拆分（前一半核心 vs 后一半核心）进行模拟。
// 在所有核心相同的统一 x86 机器上，两个"集群"应显示相似的性能 —
// 这本身就是一个有趣的发现。

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

// ─── 实验 3：混合调度 ──────────────────────────────────────────────
//
// 在 Windows 上，"大核"和"小核"通过将核心对半分来模拟：
//   - 前一半  → "小核"（场景 B）
//   - 后一半  → "大核"（场景 A）
//   - 两半一起 → "混合"（场景 C）
//
// 这在 Android 上有真实集群 ID 的情况下意义较小，
// 但仍能演示调度模式。

static void run_hybrid_test(const char* platform, int upper_bound, int runs) {
    int core_count = win_get_core_count();
    int half = core_count / 2;

    printf("\n=== Hybrid Scheduling Test (%s, %d cores) ===\n", platform, core_count);
    printf("scenario,thread_count,median_ms,min_ms,max_ms\n");

    // 场景 A：所有线程在"大核"（核心后一半）上
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

    // 场景 B：所有线程在"小核"（核心前一半）上
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

    // 场景 C：混合 — 前一半重负载，后一半轻负载
    {
        int total_n = core_count;
        double* timings = (double*)malloc(runs * sizeof(double));
        for (int run = 0; run < runs; ++run) {
            void** handles = (void**)malloc(total_n * sizeof(void*));
            prime_task_t* tasks = (prime_task_t*)malloc(total_n * sizeof(prime_task_t));

            // 前一半：重工作负载
            int chunk_heavy = upper_bound / (total_n / 2);
            for (int t = 0; t < total_n / 2; ++t) {
                tasks[t].lo = t * chunk_heavy;
                tasks[t].hi = (t + 1) * chunk_heavy - 1;
                if (t == total_n/2 - 1) tasks[t].hi = upper_bound - 1;
                tasks[t].result = (int*)malloc(sizeof(int));
                handles[t] = win_thread_create(prime_worker, &tasks[t]);
            }
            // 后一半：轻工作负载
            for (int t = total_n / 2; t < total_n; ++t) {
                tasks[t].lo = 0;
                tasks[t].hi = 1000;  // 极小
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

// ─── 主入口 ───────────────────────────────────────────────────────
// 命令行用法：
//   threadtest.exe [platform] [upper_bound] [runs]
//
// 默认值：
//   platform     = "windows"
//   upper_bound  = 1000000  （统计 [0, upper_bound) 范围内的素数）
//   runs         = 10        （每配置的迭代次数）
//
// 输出：CSV 到 stdout，与 Android 版本格式相同。

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

    // 依次执行三个实验
    printf("=== Experiment 1: Prime Counting (Single vs Multi-thread) ===\n");
    run_prime_experiment(platform, upper_bound, runs);

    printf("\n=== Experiment 2: Big.LITTLE Simulation ===\n");
    run_big_little_test(platform, upper_bound, runs);

    printf("\n=== Experiment 3: Hybrid Scheduling ===\n");
    run_hybrid_test(platform, upper_bound, runs);

    return 0;
}
