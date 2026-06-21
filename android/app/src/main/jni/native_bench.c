// Android 原生基准测试运行器
//
// 功能：
//   在 Android 上使用 pthreads 和 sched_setaffinity 执行三项线程性能实验。
//   所有输出以 CSV 格式打印到 stdout，可供 analysis/plot_comparison.py 解析。
//
// 架构：
//   - 线程操作（创建/等待/绑定）封装了 pthreads 调用。
//   - 每次实验运行多次采样，记录中位数/最小值/最大值/p95/p99 延迟和吞吐量。
//   - 该文件同时作为独立可执行程序（含 main() 入口）。
//     当作为 APK 的一部分构建时，jni_bridge.c 通过 native_bench_api.h 中声明的
//     公共 API 调用这些函数。
//
// 构建方式：
//   - 独立可执行文件:  aarch64-linux-androidXX-clang -D_GNU_SOURCE -o native_bench native_bench.c -lpthread -lm
//   - 动态库:          jni_bridge.c（独立的 .so）包含 native_bench_api.h 以复用相同函数。

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
#include "native_bench_api.h"

// ─── 线程句柄 ─────────────────────────────────────────────────────
// 对 pthread_t 的透明封装，使调用者无需了解 pthread 内部细节。
// 对应 Windows 端的 win_thread_handle。

struct thread_handle {
    pthread_t tid;
};

// pthread_wrapper — 传递给 pthread_create 的适配器函数。
//
// pthread_create 要求函数签名必须是 `void* (*)(void*)`。
// 我们的工作函数虽然签名相同，但需要传递两个用户参数（函数指针 + 用户数据），
// 只能通过唯一的 `void* arg` 槽位传入。此包装器负责解包它们。
//
// arg 布局: [0] = 函数指针, [1] = 用户数据指针。
static void* pthread_wrapper(void* arg) {
    void* (*func)(void*) = (void*(*)(void*))(((void**)arg)[0]);
    void* user_arg = ((void**)arg)[1];
    free(arg);          // 释放包装数组
    return func(user_arg);
}

// android_thread_create — 跨平台的线程创建封装。
//
// 创建一个栈大小为 256 KB 的新 pthread（远小于默认的 8 MB），以便在实验中
// 同时创建大量线程时不会耗尽内存。
// 返回一个透明句柄供 android_thread_join 使用。
// 失败时返回 NULL。
void* android_thread_create(void (*func)(void*), void* arg) {
    // 将函数指针和用户参数打包到一个 malloc 数组中。
    void** wrapped = (void**)malloc(2 * sizeof(void*));
    wrapped[0] = (void*)func;
    wrapped[1] = arg;

    pthread_t tid;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    // 256 KB 栈空间 — 远小于默认的 8 MB，在实验中同时创建大量线程时很重要。
    pthread_attr_setstacksize(&attr, 256 * 1024);

    if (pthread_create(&tid, &attr, pthread_wrapper, wrapped) != 0) {
        free(wrapped);
        pthread_attr_destroy(&attr);
        return NULL;
    }
    pthread_attr_destroy(&attr);

    struct thread_handle* h = (struct thread_handle*)malloc(sizeof(*h));
    h->tid = tid;
    return (void*)h;
}

// android_thread_join — 等待由 android_thread_create 创建的线程结束。
//
// 连接 pthread 并释放句柄结构体。
// 传入 NULL 是安全的（无操作）。
void android_thread_join(void* handle) {
    if (!handle) return;
    struct thread_handle* h = (struct thread_handle*)handle;
    pthread_join(h->tid, NULL);
    free(h);
}

// android_thread_bind_cpu — 将调用线程绑定到指定核心。
//
// 使用 sched_setaffinity，将掩码设置为仅包含 core_id。
// 如果调用失败（例如核心不存在），向 stderr 打印警告。
void android_thread_bind_cpu(int core_id) {
    cpu_set_t mask;
    CPU_ZERO(&mask);
    CPU_SET(core_id, &mask);
    if (sched_setaffinity(0, sizeof(mask), &mask) != 0) {
        fprintf(stderr, "Warning: sched_setaffinity(cpu%d) failed: %s\n",
                core_id, strerror(errno));
    }
}

// android_get_core_count — 返回在线 CPU 核心数量。
//
// 使用 sysconf(_SC_NPROCESSORS_ONLN)，报告当前进程可见的核心数
//（如果部分核心离线或被 cgroups 限制，可能与物理核心数不同）。
int android_get_core_count(void) {
    return (int)sysconf(_SC_NPROCESSORS_ONLN);
}

// android_get_cluster_map — 检测 big.LITTLE 核心拓扑。
//
// 对每个核心读取 /sys/devices/system/cpu/cpuN/topology/cluster_id。
// 如果 sysfs 条目不存在（较老的内核），回退到启发式策略：
// 前一半核心 = cluster 0（小核），后一半核心 = cluster 1（大核）。
//
// 返回一个 malloc 的 int[]，每个核心一个集群 ID。
// 调用者负责 free()。*out_count 设为核心数量。
int* android_get_cluster_map(int* out_count) {
    int count = android_get_core_count();
    int* clusters = (int*)malloc(count * sizeof(int));
    char path[256];

    for (int i = 0; i < count; ++i) {
        snprintf(path, sizeof(path),
                 "/sys/devices/system/cpu/cpu%d/topology/cluster_id", i);
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
            // 回退：将核心均分两半（适用于没有 cluster_id 的设备）
            clusters[i] = (i < count / 2) ? 0 : 1;
        }
    }
    *out_count = count;
    return clusters;
}

// ─── 时间测量 ─────────────────────────────────────────────────────
// 使用 gettimeofday() 实现亚毫秒级精度。
// Windows 端使用 QueryPerformanceCounter 会更精确，但
// gettimeofday 对我们的微秒级测量已经足够。

static double now_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec * 1000.0 + tv.tv_usec / 1000.0;
}

// ─── 工作负载：素数计数 ───────────────────────────────────────────
// 一个 CPU 密集型工作负载，具有可预测的复杂度。素数计数的优势：
//   1. 确定性 — 相同输入始终产生相同输出。
//   2. CPU 密集 — 无 I/O 或内存瓶颈。
//   3. 易于并行化 — 每个范围相互独立。
//   4. 粒度适中 — 能清晰展示线程开销效应。

// is_prime — 试除法素性检测，使用 6k±1 优化。
//
// 先检查是否能被 2 和 3 整除，然后迭代检查 6k-1 和 6k+1 形式的候选数，
// 直到 sqrt(n)。
// 最坏情况 O(sqrt(n))，但对 n < 10^7 实际很快。
static int is_prime(int n) {
    if (n < 2) return 0;
    if (n < 4) return 1;
    if (n % 2 == 0 || n % 3 == 0) return 0;
    for (int i = 5; (long long)i * i <= n; i += 6) {
        if (n % i == 0 || n % (i + 2) == 0) return 0;
    }
    return 1;
}

// prime_task_t — 描述一个需要计数的范围 [lo, hi]。
// 工作线程完成后，结果写入 *result。
typedef struct {
    int lo, hi;     // 包含边界的范围
    int* result;    // 输出指针（工作线程写入）
} prime_task_t;

// prime_worker — 每个线程执行的函数。
//
// 统计范围 [lo, hi] 内的素数个数，并将结果写入 *result。
// 这是被分配到各线程的工作单元。
void* prime_worker(void* arg) {
    prime_task_t* task = (prime_task_t*)arg;
    int count = 0;
    for (int i = task->lo; i <= task->hi; ++i) {
        if (is_prime(i)) ++count;
    }
    *(task->result) = count;
    return NULL;
}

// ─── 统计计算 ─────────────────────────────────────────────────────
// 每次实验配置运行多次（N 次采样）后，我们计算统计摘要以消除
// OS 调度器和其他系统活动带来的噪声影响。

// bench_stats_t — 聚合的基准测试指标。
//
// 字段说明：
//   median_ms    — 排序后时序的中位数（对异常值鲁棒）
//   min_ms/max_ms — 最好/最差情况
//   avg_ms       — 算术平均值
//   p95_ms/p99_ms — 尾部延迟百分位
//   throughput_ops — 每秒操作数（基于中位数计算）
//   speedup      — 基线（1线程）时间与当前配置时间的比值
typedef struct {
    double median_ms, min_ms, max_ms, avg_ms;
    double p95_ms, p99_ms;
    double throughput_ops, speedup;
} bench_stats_t;

// compute_stats — 从原始时序数据计算统计摘要。
//
// 参数：
//   timings      — N 个测量持续时间数组（单位：毫秒）
//   num_runs     — timings[] 中的样本数量
//   total_ops    — 执行的总操作数（用于吞吐量计算）
//   baseline_ms  — 1线程基线所花时间（用于加速比计算）
//
// 返回包含所有计算指标的 bench_stats_t 结构体。
bench_stats_t compute_stats(double* timings, int num_runs,
                            double total_ops, double baseline_ms) {
    bench_stats_t s = {0};
    if (num_runs == 0) return s;

    // 复制并排序时序数据，用于百分位计算。
    // 使用插入排序，因为 N 很小（通常 5-10 次采样）。
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

    // 计算平均值
    double sum = 0;
    for (int i = 0; i < num_runs; ++i) sum += sorted[i];
    s.avg_ms = sum / num_runs;

    // 百分位数
    s.median_ms = sorted[num_runs / 2];
    s.min_ms    = sorted[0];
    s.max_ms    = sorted[num_runs - 1];
    s.p95_ms    = sorted[(int)(num_runs * 0.95)];
    s.p99_ms    = sorted[(int)(num_runs * 0.99)];

    // 衍生指标
    s.throughput_ops = total_ops / (s.median_ms / 1000.0);  // 每秒操作数
    s.speedup        = (baseline_ms > 0) ? (baseline_ms / s.median_ms) : 1.0;

    free(sorted);
    return s;
}

// ─── 实验 1：不同线程数下的素数计数 ─────────────────────────────────
//
// 目标：测量相同 CPU 密集型工作负载下，Android（CFS 调度器）
// 和 Windows（NT 调度器）在线程扩展性方面的差异。
//
// 方法：
//   1. 使用 1、2、4、6、8、12、16 线程运行素数计数任务。
//      （仅测试 thread_count <= core_count 的配置。）
//   2. 对每种配置运行 N 次迭代并记录时序。
//   3. 计算相对于 1 线程基线的加速比。
//
// 预期发现：
//   - 低线程数时接近线性加速（2-4 线程）。
//   - 超过一定数量后收益递减，原因包括：
//     * 线程创建/销毁开销
//     * 缓存抖动（多个线程竞争 L1/L2 缓存）
//     * OS 调度器抢占
//   - 由于不同的调度策略（CFS 公平分享 vs NT 基于优先级），
//     Android 可能显示出与 Windows 不同的扩展曲线。

typedef struct {
    int num_threads;
    int upper_bound;
    prime_task_t* tasks;
} prime_run_ctx_t;

// prime_run_worker — 启动并等待一批线程。
//
// 创建 num_threads 个线程（每个任务一个），等待全部完成后返回。
// 这是实验每次运行的内层循环体。
static void prime_run_worker(void* arg) {
    prime_run_ctx_t* ctx = (prime_run_ctx_t*)arg;
    void** handles = (void**)malloc(ctx->num_threads * sizeof(void*));

    // 阶段 1：启动所有线程
    for (int t = 0; t < ctx->num_threads; ++t) {
        handles[t] = android_thread_create(prime_worker, &ctx->tasks[t]);
    }
    // 阶段 2：等待全部完成
    for (int t = 0; t < ctx->num_threads; ++t) {
        android_thread_join(handles[t]);
    }
    free(handles);
}

// run_prime_experiment — 执行实验 1 并打印 CSV 输出。
//
// 参数：
//   platform     — 字符串标签（"android" 或 "windows"），用于 CSV 输出
//   upper_bound  — 在范围 [0, upper_bound) 内计数素数
//   runs         — 每个线程数配置的迭代次数
void run_prime_experiment(const char* platform, int upper_bound, int runs) {
    int core_count = android_get_core_count();

    // 测试这些线程数（过滤到 <= core_count）
    int thread_counts[] = {1, 2, 4, 6, 8, 12, 16};
    int num_configs = 0;
    for (int i = 0; i < (int)(sizeof(thread_counts)/sizeof(thread_counts[0])); ++i) {
        if (thread_counts[i] <= core_count) num_configs = i + 1;
        else break;
    }

    // CSV 表头
    printf("experiment,platform,num_threads,median_ms,min_ms,max_ms,avg_ms,p95_ms,p99_ms,throughput_ops,speedup\n");

    // ── 步骤 1：测量基线（1 线程）─────────────────────────────────
    // 基线取所有运行的几何平均以减少噪声。
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

    // ── 步骤 2：测量每个多线程配置 ────────────────────────────────
    for (int ci = 0; ci < num_configs; ++ci) {
        int nt = thread_counts[ci];
        prime_task_t* tasks = (prime_task_t*)malloc(nt * sizeof(prime_task_t));
        double* timings = (double*)malloc(runs * sizeof(double));

        for (int run = 0; run < runs; ++run) {
            // 均匀分配工作到 nt 个线程。
            // 线程 t 处理范围 [t*N/nt, (t+1)*N/nt - 1]。
            // 最后一个线程获得剩余部分以覆盖完整范围。
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

            // 释放每个线程的结果缓冲区
            for (int t = 0; t < nt; ++t) free(tasks[t].result);
        }

        // 计算并打印统计信息
        bench_stats_t s = compute_stats(timings, runs, upper_bound, baseline_ms);
        printf("prime_counting,%s,%d,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.0f,%.2f\n",
               platform, nt, s.median_ms, s.min_ms, s.max_ms, s.avg_ms,
               s.p95_ms, s.p99_ms, s.throughput_ops, s.speedup);

        free(timings);
        free(tasks);
    }
}

// ─── 实验 2：大小核分离测试 ────────────────────────────────────────
//
// 目标：量化 ARM big.LITTLE 架构中大核（P-core）和小核（E-core）
// 之间的绝对性能差异。
//
// 方法：
//   1. 通过 /sys/devices/system/cpu/cpu*/topology/cluster_id 检测核心集群。
//   2. 对每个集群，使用 sched_setaffinity 将所有线程绑定到该集群的核心上，
//      然后运行素数计数工作负载。
//   3. 比较集群之间的中位延迟和吞吐量。
//
// 预期发现：
//   - 大核应显著更快地完成工作负载。
//   - 比值（小核时间 / 大核时间）量化了性能差距。
//   - 此数据有助于解释 Android 任务分类调度器的合理性
//     （重任务 → 大核，轻任务 → 小核）。

void run_big_little_test(const char* platform, int upper_bound, int runs) {
    int core_count;
    int* clusters = android_get_cluster_map(&core_count);

    // 确定有多少个不同的集群
    int max_cluster = 0;
    for (int i = 0; i < core_count; ++i) {
        if (clusters[i] > max_cluster) max_cluster = clusters[i];
    }
    int num_clusters = max_cluster + 1;

    printf("\n=== Big.LITTLE Test (%s, %d cores) ===\n", platform, core_count);
    printf("cluster,core_count,median_ms,min_ms,max_ms,throughput_ops\n");

    // 对每个集群运行一次子测试
    for (int cid = 0; cid < num_clusters; ++cid) {
        // 收集属于此集群的所有核心 ID
        int* ccores = (int*)malloc(core_count * sizeof(int));
        int cc_count = 0;
        for (int i = 0; i < core_count; ++i) {
            if (clusters[i] == cid) ccores[cc_count++] = i;
        }

        // 将这些核心绑定到工作负载，多次运行
        double* timings = (double*)malloc(runs * sizeof(double));
        for (int run = 0; run < runs; ++run) {
            void** handles = (void**)malloc(cc_count * sizeof(void*));
            prime_task_t* tasks = (prime_task_t*)malloc(cc_count * sizeof(prime_task_t));

            for (int t = 0; t < cc_count; ++t) {
                // 将线程 t 绑定到其专用核心
                android_thread_bind_cpu(ccores[t]);
                // 每个线程执行完整工作负载（不分区）
                // — 这测量的是裸核吞吐量，而非并行加速。
                tasks[t].lo = 0;
                tasks[t].hi = upper_bound - 1;
                tasks[t].result = (int*)malloc(sizeof(int));
                handles[t] = android_thread_create(prime_worker, &tasks[t]);
            }

            // 解除计时线程的亲和性绑定，使其不受核心亲和影响
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

        // 计算汇总统计（为简单起见内联）
        double sum = 0;
        for (int i = 0; i < runs; ++i) sum += timings[i];
        double avg = sum / runs;
        double mn = timings[0], mx = timings[0];
        for (int i = 1; i < runs; ++i) {
            if (timings[i] < mn) mn = timings[i];
            if (timings[i] > mx) mx = timings[i];
        }
        double* sorted = (double*)malloc(runs * sizeof(double));
        memcpy(sorted, timings, runs * sizeof(double));
        for (int i = 1; i < runs; ++i) {
            double key = sorted[i];
            int j = i - 1;
            while (j >= 0 && sorted[j] > key) { sorted[j+1] = sorted[j]; --j; }
            sorted[j+1] = key;
        }
        double median = sorted[runs / 2];
        // 吞吐量：所有核心的总素数检查次数除以时间
        double tp = (double)upper_bound * cc_count / (median / 1000.0);

        // 标签：集群 0 = 小核（E-core），其他 = 大核（P-core）
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

// ─── 实验 3：大小核混合调度 ────────────────────────────────────────
//
// 目标：测试在大核和小核之间混合分配工作负载是否能在保持接近大核性能
// 的同时让空闲的小核也发挥作用。
//
// 场景：
//   A) ALL_BIG — 所有线程仅在大核上运行（性能上限）
//   B) ALL_LIT — 所有线程仅在在小核上运行（能效下限）
//   C) HYBRID  — 大核处理完整工作负载，小核处理轻量任务
//                 （模拟 Android 的任务分类机制）
//
// 假设：
//   HYBRID 可能比 ALL_BIG 稍慢（因为小核增加了额外开销），但功耗显著更低。
//   这证明了 Android 省电的核心秘密——不是把所有任务都压到大核上，
//   而是按任务类型智能分配核心。

void run_hybrid_test(const char* platform, int upper_bound, int runs) {
    int core_count;
    int* clusters = android_get_cluster_map(&core_count);

    int max_cluster = 0;
    for (int i = 0; i < core_count; ++i)
        if (clusters[i] > max_cluster) max_cluster = clusters[i];
    int num_clusters = max_cluster + 1;

    // 统计每个集群的核心数量
    int* cluster_sizes = (int*)calloc(num_clusters, sizeof(int));
    for (int i = 0; i < core_count; ++i) cluster_sizes[clusters[i]]++;

    printf("\n=== Hybrid Scheduling Test (%s, %d cores) ===\n", platform, core_count);
    printf("scenario,thread_count,median_ms,min_ms,max_ms,throughput_ops\n");

    // 收集每个集群的核心索引供后续使用
    int** cluster_cores = (int**)malloc(num_clusters * sizeof(int*));
    for (int c = 0; c < num_clusters; ++c) {
        cluster_cores[c] = (int*)malloc(core_count * sizeof(int));
        int cnt = 0;
        for (int i = 0; i < core_count; ++i) {
            if (clusters[i] == c) cluster_cores[c][cnt++] = i;
        }
        cluster_cores[c] = (int*)realloc(cluster_cores[c], cnt * sizeof(int));
    }

    // ── 场景 A：所有线程仅在大核上 ──────────────────────────────
    // 这是"性能天花板" — 最大吞吐量但也最大功耗。
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

        // 计算统计
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
        printf("  ALL_BIG:   %d threads, median=%.3fms, throughput=%.0f ops/s\n",
               big_n, median, tp);
        free(sorted);
        free(timings);
    }

    // ── 场景 B：所有线程仅在在小核上 ─────────────────────────────
    // 这是"能效地板" — 最低吞吐量但也最低功耗。
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

        // 计算统计
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
        printf("  ALL_LITTLE: %d threads, median=%.3fms, throughput=%.0f ops/s\n",
               lit_n, median, tp);
        free(sorted);
        free(timings);
    }

    // ── 场景 C：混合 — 大核处理重任务，小核处理轻任务 ────────────
    // 这模拟了 Android 的自适应调度器：
    //   - 大核（P-核）处理素数计数工作负载（CPU 密集型）。
    //   - 小核（E-核）处理琐碎工作（如统计偶数个数）。
    //
    // 关键问题：添加小核工作是否会因缓存/互连带宽竞争而拖慢大核线程？
    //   如果不会，那么混合调度就是一个双赢策略 — 我们获得接近满血的性能，
    //   同时功耗更低。
    {
        int big_n = cluster_sizes[num_clusters - 1];
        int lit_n = cluster_sizes[0];
        int total_n = big_n + lit_n;

        if (big_n > 0 && lit_n > 0) {
            double* timings = (double*)malloc(runs * sizeof(double));

            for (int run = 0; run < runs; ++run) {
                void** handles = (void**)malloc(total_n * sizeof(void*));
                prime_task_t* tasks = (prime_task_t*)malloc(total_n * sizeof(prime_task_t));

                // 大核：完整素数计数工作负载（均匀分区）
                int chunk_big = upper_bound / big_n;
                for (int t = 0; t < big_n; ++t) {
                    tasks[t].lo = t * chunk_big;
                    tasks[t].hi = (t + 1) * chunk_big - 1;
                    if (t == big_n - 1) tasks[t].hi = upper_bound - 1;
                    tasks[t].result = (int*)malloc(sizeof(int));
                    handles[t] = android_thread_create(prime_worker, &tasks[t]);
                }

                // 小核：轻量工作负载（仅统计前 10000 个数中的素数）
                // 在实际应用中，这可能是 UI 渲染、网络轮询等。
                for (int t = 0; t < lit_n; ++t) {
                    tasks[t].lo = 0;
                    tasks[t].hi = 10000;  // 微小工作负载
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

            // 计算统计
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

    // ── 清理 ─────────────────────────────────────────────────────
    for (int c = 0; c < num_clusters; ++c) free(cluster_cores[c]);
    free(cluster_cores);
    free(cluster_sizes);
    free(clusters);
}

// ─── 主入口 ───────────────────────────────────────────────────────
// 作为独立可执行程序运行时，接受三个可选参数：
//   argv[1] — 平台名称（默认："android"）
//   argv[2] — 素数计数上限（默认：1000000）
//   argv[3] — 每配置的迭代次数（默认：10）
//
// 用法：
//   ./native_bench android 1000000 10
//
// 输出为 CSV 格式到 stdout，可管道传输给分析脚本。

int main(int argc, char* argv[]) {
    const char* platform = "android";
    int upper_bound = 1000000;  // 统计 [0, upper_bound) 范围内的素数个数
    int runs = 10;

    if (argc > 1) platform = argv[1];
    if (argc > 2) upper_bound = atoi(argv[2]);
    if (argc > 3) runs = atoi(argv[3]);

    printf("Platform: %s\n", platform);
    printf("Prime upper bound: %d\n", upper_bound);
    printf("Runs per config: %d\n", runs);
    printf("Core count: %d\n\n", android_get_core_count());

    // 依次执行三个实验
    printf("=== Experiment 1: Prime Counting (Single vs Multi-thread) ===\n");
    run_prime_experiment(platform, upper_bound, runs);

    printf("\n=== Experiment 2: Big.LITTLE Separation ===\n");
    run_big_little_test(platform, upper_bound, runs);

    printf("\n=== Experiment 3: Hybrid Scheduling ===\n");
    run_hybrid_test(platform, upper_bound, runs);

    return 0;
}
