// bench_core.h - 跨平台基准测试核心接口
//
// 提供平台无关的基准测试逻辑：
//   - 素数计数算法
//   - 统计计算
//   - 实验运行框架
//
// 平台特定代码（线程创建、CPU绑定等）通过回调函数接口实现。

#ifndef BENCH_CORE_H
#define BENCH_CORE_H

#include <vector>
#include <string>
#include <functional>
#include <cstdint>

// ─── 平台抽象回调 ────────────────────────────────────────────────
// 这些由平台特定代码实现，核心代码通过回调使用。

struct BenchCallbacks {
    // 线程相关
    std::function<void*(std::function<void(void*)>, void*)> create_thread;  // (func, arg) -> handle
    std::function<void(void*)> join_thread;                                     // handle -> void
    std::function<int()> get_core_count;                                        // -> count
    std::function<void(int)> bind_cpu;                                          // core_id -> void
    
    // 时间相关
    std::function<double()> now_ms;                                             // -> ms
    
    // 集群相关（可选，默认使用启发式）
    std::function<std::vector<int>()> get_cluster_map;                         // -> cluster_ids
};

// ─── 数据结构 ─────────────────────────────────────────────────────

struct PrimeTask {
    int lo;
    int hi;
    int* result;
};

struct BenchStats {
    double median_ms = 0;
    double min_ms = 0;
    double max_ms = 0;
    double avg_ms = 0;
    double p95_ms = 0;
    double p99_ms = 0;
    double throughput_ops = 0;
    double speedup = 1.0;
};

// ─── 核心算法 ─────────────────────────────────────────────────────

// 素数判断（6k±1优化）
bool is_prime(int n);

// 素数计数工作函数
void prime_worker(PrimeTask* task);

// 统计计算
BenchStats compute_stats(const std::vector<double>& timings,
                         double total_ops,
                         double baseline_ms);

// ─── 实验接口 ─────────────────────────────────────────────────────

// 实验1：不同线程数下的素数计数
void run_prime_experiment(const BenchCallbacks& cb,
                          const std::string& platform,
                          int upper_bound,
                          int runs);

// 实验2：大小核测试
void run_big_little_test(const BenchCallbacks& cb,
                         const std::string& platform,
                         int upper_bound,
                         int runs);

// 实验3：混合调度测试
void run_hybrid_test(const BenchCallbacks& cb,
                     const std::string& platform,
                     int upper_bound,
                     int runs);

#endif // BENCH_CORE_H