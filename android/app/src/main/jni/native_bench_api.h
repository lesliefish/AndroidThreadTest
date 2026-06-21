// native_bench_api.h — 基准测试实验的公共 API
//
// 此头文件被 BOTH（双方）包含：
//   1. native_bench.c   — 编译为独立 ARM 可执行文件（用于 adb shell）
//   2. jni_bridge.c     — 编译为 libthreadtest_jni.so（用于 Android APK）
//
// 通过共享此头文件，可执行文件和 .so 都链接到相同的实验函数，
// 确保无论通过何种方式调用，基准测试逻辑完全一致。
//
// 设计考量：
//   - 函数声明不加 `static`，以便从其他翻译单元（jni_bridge.c）调用。
//   - 数据结构（prime_task_t, bench_stats_t）在此定义，
//     确保两个文件对它们的布局达成一致。
//   - 内部辅助函数（is_prime, now_ms, pthread_wrapper）在
//     native_bench.c 中保持 static，不对外暴露。

#ifndef NATIVE_BENCH_API_H
#define NATIVE_BENCH_API_H

// ─── 数据结构 ─────────────────────────────────────────────────────

// prime_task_t — 描述一个需要统计素数的整数范围 [lo, hi]。
//
// 字段：
//   lo      — 范围的包含性下界
//   hi      — 范围的包含性上界
//   result  — 输出指针，工作线程完成后写入素数计数
//
// 独立可执行文件和 JNI 桥接层都使用它来在线程间分配工作。
typedef struct {
    int lo, hi;
    int* result;
} prime_task_t;

// bench_stats_t — 基准测试运行的聚合统计数据。
//
// 字段：
//   median_ms     — 中位执行时间（最具代表性的单一指标）
//   min_ms        — 所有运行中的最佳时间
//   max_ms        — 所有运行中的最差时间
//   avg_ms        — 所有运行的算术平均
//   p95_ms        — 第 95 百分位（尾部延迟指示器）
//   p99_ms        — 第 99 百分位（极端尾部延迟）
//   throughput_ops — 每秒操作数（从中位数推导）
//   speedup       — 基线时间与当前配置时间的比值（加速比）
//
// 由 compute_stats() 在收集多次运行的时序数据后计算得出。
typedef struct {
    double median_ms, min_ms, max_ms, avg_ms;
    double p95_ms, p99_ms;
    double throughput_ops, speedup;
} bench_stats_t;

// ─── 线程操作 ─────────────────────────────────────────────────────
// 跨平台的线程 API 封装。
// 在 native_bench.c 中实现（Android/pthreads），
// 通过此头文件被 jni_bridge.c 引用。

// android_thread_create — 创建一个新线程执行 `func`，传入 `arg`。
//
// 参数：
//   func — 工作函数指针（接收 void*，返回 void*）
//   arg  — 传递给 func 的用户数据
//
// 返回值：
//   供 android_thread_join 使用的透明句柄，失败时返回 NULL。
//
// 实现细节：
//   - 栈大小设为 256 KB（远小于默认 8 MB），
//     允许在不消耗过多内存的情况下创建大量线程。
//   - 函数指针和参数被打包进一个 malloc 数组，
//     通过 pthread_wrapper（适配签名）传递。
void*  android_thread_create(void* (*func)(void*), void* arg);

// android_thread_join — 等待线程完成并释放其句柄。
//
// 参数：
//   handle — android_thread_create 返回的透明句柄
//
// 传入 NULL 是安全的（无操作）。
void   android_thread_join(void* handle);

// android_thread_bind_cpu — 将调用线程绑定到指定 CPU 核心。
//
// 参数：
//   core_id — 目标核心的从零开始的索引
//
// 底层使用 sched_setaffinity()。如果核心不存在或调用失败，
// 向 stderr 打印警告。
void  android_thread_bind_cpu(int core_id);

// android_get_core_count — 返回在线 CPU 核心数量。
//
// 使用 sysconf(_SC_NPROCESSORS_ONLN)，报告当前进程可见的核心数。
// 如果部分核心离线或应用被 cgroups 限制，可能与物理核心数不同。
int   android_get_core_count(void);

// android_get_cluster_map — 检测 big.LITTLE 核心拓扑。
//
// 参数：
//   out_count — 返回时设为核心数量
//
// 返回值：
//   malloc 的 int[]，每个核心一个集群 ID。
//   集群 0 = 小核（E-core），集群 1+ = 大核（P-core）。
//
// 检测方法：
//   1. 尝试读取 /sys/devices/system/cpu/cpuN/topology/cluster_id
//   2. 回退到启发式：前一半 = 集群 0，后一半 = 集群 1
int*  android_get_cluster_map(int* out_count);

// ─── 工作负载 ─────────────────────────────────────────────────────

// prime_worker — 统计范围 [task->lo, task->hi] 内的素数个数。
//
// 这是并行工作的基本单元。每个线程使用描述其分配范围的
// prime_task_t 调用此函数。
//
// 返回 NULL（pthread 约定）。
void* prime_worker(void* arg);

// ─── 统计 ─────────────────────────────────────────────────────────

// compute_stats — 从原始时序数据计算统计摘要。
//
// 参数：
//   timings      — N 个测量持续时间数组（单位：毫秒）
//   num_runs     — timings[] 中的样本数量
//   total_ops    — 执行的总操作数（用于吞吐量计算）
//   baseline_ms  — 1线程基线所花时间（用于加速比计算）
//
// 返回包含中位数、百分位、吞吐量和加速比的 bench_stats_t 结构体。
bench_stats_t compute_stats(double* timings, int num_runs,
                            double total_ops, double baseline_ms);

// ─── 实验函数 ─────────────────────────────────────────────────────
// 每个函数运行一项实验并向 stdout 打印 CSV 行。
// 设计为可从独立 main() 和 JNI 桥接层同时调用。

// run_prime_experiment — 实验 1：测量不同线程数下的扩展性。
//
// 测试线程数：1、2、4、6、8、12、16（过滤到 <= core_count）。
// 对每种配置，计算相对于 1 线程基线的加速比。
//
// 参数：
//   platform     — CSV 输出的字符串标签（"android" 或 "windows"）
//   upper_bound  — 在范围 [0, upper_bound) 内计数素数
//   runs         — 每个线程数配置的迭代次数
void run_prime_experiment(const char* platform, int upper_bound, int runs);

// run_big_little_test — 实验 2：比较大核与小核的性能。
//
// 对每个检测到的核心集群：
//   1. 为集群中的每个核心绑定一个线程
//   2. 在每个核心上运行完整素数计数工作负载（不分区）
//   3. 测量中位延迟和吞吐量
//
// 参数：
//   platform     — CSV 输出的字符串标签
//   upper_bound  — 素数计数范围
//   runs         — 每个集群的迭代次数
void run_big_little_test(const char* platform, int upper_bound, int runs);

// run_hybrid_test — 实验 3：测试混合大小核调度。
//
// 运行三种场景：
//   ALL_BIG  — 所有线程在大核上（性能上限）
//   ALL_LIT  — 所有线程在小核上（能效下限）
//   HYBRID   — 大核处理重任务，小核处理轻任务
//
// 参数：
//   platform     — CSV 输出的字符串标签
//   upper_bound  — 素数计数范围
//   runs         — 每种场景的迭代次数
void run_hybrid_test(const char* platform, int upper_bound, int runs);

#endif // NATIVE_BENCH_API_H
