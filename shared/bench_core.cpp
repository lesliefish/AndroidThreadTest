// bench_core.cpp - 跨平台基准测试核心实现
//
// 包含平台无关的基准测试逻辑：
//   - 素数计数算法
//   - 统计计算
//   - 三项实验的实现

#include "bench_core.h"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <numeric>
#include <sstream>

// ─── 核心算法 ─────────────────────────────────────────────────────

bool is_prime(int n) {
    if (n < 2) return false;
    if (n < 4) return true;
    if (n % 2 == 0 || n % 3 == 0) return false;
    for (int i = 5; static_cast<long long>(i) * i <= n; i += 6) {
        if (n % i == 0 || n % (i + 2) == 0) return false;
    }
    return true;
}

void prime_worker(PrimeTask* task) {
    int count = 0;
    for (int i = task->lo; i <= task->hi; ++i) {
        if (is_prime(i)) ++count;
    }
    *(task->result) = count;
}

BenchStats compute_stats(const std::vector<double>& timings,
                         double total_ops,
                         double baseline_ms) {
    BenchStats s;
    if (timings.empty()) return s;

    auto sorted = timings;
    std::sort(sorted.begin(), sorted.end());

    double sum = 0;
    for (const auto t : sorted) {
        sum += t;
    }
    s.avg_ms = sum / sorted.size();
    s.median_ms = sorted[sorted.size() / 2];
    s.min_ms = sorted.front();
    s.max_ms = sorted.back();
    s.p95_ms = sorted[static_cast<size_t>(sorted.size() * 0.95)];
    s.p99_ms = sorted[static_cast<size_t>(sorted.size() * 0.99)];
    s.throughput_ops = total_ops / (s.median_ms / 1000.0);
    s.speedup = (baseline_ms > 0) ? (baseline_ms / s.median_ms) : 1.0;

    return s;
}

// ─── 实验实现 ─────────────────────────────────────────────────────

void run_prime_experiment(const BenchCallbacks& cb,
                          const std::string& platform,
                          int upper_bound,
                          int runs) {
    int core_count = cb.get_core_count();
    
    std::vector<int> thread_counts = {1, 2, 4, 6, 8, 12, 16};
    // 过滤出 <= core_count 的配置
    thread_counts.erase(
        std::remove_if(thread_counts.begin(), thread_counts.end(),
                       [core_count](int t) { return t > core_count; }),
        thread_counts.end());

    std::cout << "experiment,platform,num_threads,median_ms,min_ms,max_ms,avg_ms,p95_ms,p99_ms,throughput_ops,speedup\n";

    // 基线：1 线程
    PrimeTask baseline_task;
    baseline_task.lo = 0;
    baseline_task.hi = upper_bound - 1;
    int baseline_result = 0;
    baseline_task.result = &baseline_result;

    double baseline_ms = 0;
    for (int run = 0; run < runs; ++run) {
        baseline_task.result = &baseline_result;
        double start = cb.now_ms();
        prime_worker(&baseline_task);
        baseline_ms = cb.now_ms() - start;
    }

    // 多线程运行
    for (int nt : thread_counts) {
        std::vector<PrimeTask> tasks(nt);
        std::vector<double> timings(runs);

        for (int run = 0; run < runs; ++run) {
            for (int t = 0; t < nt; ++t) {
                tasks[t].lo = t * upper_bound / nt;
                tasks[t].hi = (t + 1) * upper_bound / nt - 1;
                if (t == nt - 1) tasks[t].hi = upper_bound - 1;
                tasks[t].result = new int;
            }

            // 创建并等待所有线程
            std::vector<void*> handles;
            handles.reserve(nt);
            for (int t = 0; t < nt; ++t) {
                handles.push_back(cb.create_thread([](void* arg) {
                    prime_worker(static_cast<PrimeTask*>(arg));
                }, &tasks[t]));
            }

            double start = cb.now_ms();
            for (auto h : handles) {
                cb.join_thread(h);
            }
            timings[run] = cb.now_ms() - start;

            for (int t = 0; t < nt; ++t) {
                delete tasks[t].result;
            }
        }

        BenchStats stats = compute_stats(timings, upper_bound, baseline_ms);
        std::cout << "prime_counting," << platform << "," << nt << ","
                  << stats.median_ms << "," << stats.min_ms << "," << stats.max_ms << ","
                  << stats.avg_ms << "," << stats.p95_ms << "," << stats.p99_ms << ","
                  << stats.throughput_ops << "," << stats.speedup << "\n";
    }
}

void run_big_little_test(const BenchCallbacks& cb,
                         const std::string& platform,
                         int upper_bound,
                         int runs) {
    std::vector<int> clusters = cb.get_cluster_map();
    int core_count = static_cast<int>(clusters.size());

    int max_cluster = 0;
    for (int c : clusters) {
        if (c > max_cluster) max_cluster = c;
    }
    int num_clusters = max_cluster + 1;

    std::cout << "\n=== Big.LITTLE Simulation (" << platform << ", " << core_count << " cores) ===\n";
    std::cout << "(Using heuristic cluster detection)\n";
    std::cout << "cluster,core_count,median_ms,min_ms,max_ms,throughput_ops\n";

    for (int cid = 0; cid < num_clusters; ++cid) {
        std::vector<int> ccores;
        for (int i = 0; i < core_count; ++i) {
            if (clusters[i] == cid) ccores.push_back(i);
        }

        std::vector<double> timings(runs);
        for (int run = 0; run < runs; ++run) {
            int cc_count = static_cast<int>(ccores.size());
            std::vector<PrimeTask> tasks(cc_count);
            std::vector<int*> results(cc_count);
            std::vector<void*> handles;
            handles.reserve(cc_count);

            int chunk = upper_bound / cc_count;
            for (int t = 0; t < cc_count; ++t) {
                tasks[t].lo = t * chunk;
                tasks[t].hi = (t + 1) * chunk - 1;
                if (t == cc_count - 1) tasks[t].hi = upper_bound - 1;
                results[t] = new int;
                tasks[t].result = results[t];
                handles.push_back(cb.create_thread([](void* arg) {
                    prime_worker(static_cast<PrimeTask*>(arg));
                }, &tasks[t]));
            }

            double start = cb.now_ms();
            for (auto h : handles) {
                cb.join_thread(h);
            }
            timings[run] = cb.now_ms() - start;

            for (int* r : results) {
                delete r;
            }
        }

        double mn = timings[0], mx = timings[0];
        auto sorted = timings;
        std::sort(sorted.begin(), sorted.end());
        double median = sorted[sorted.size() / 2];
        double tp = static_cast<double>(upper_bound) * static_cast<double>(ccores.size()) / (median / 1000.0);

        std::string cname = (cid == 0) ? "cluster0" : "cluster1";
        std::cout << cname << "," << ccores.size() << "," << median << "," << mn << "," << mx << "," << tp << "\n";
        std::cout << "  " << cname << " (" << ccores.size() << " cores): median=" << median << "ms  throughput=" << tp << " ops/s\n";
    }
}

void run_hybrid_test(const BenchCallbacks& cb,
                     const std::string& platform,
                     int upper_bound,
                     int runs) {
    int core_count = cb.get_core_count();
    int half = core_count / 2;

    std::cout << "\n=== Hybrid Scheduling Test (" << platform << ", " << core_count << " cores) ===\n";
    std::cout << "scenario,thread_count,median_ms,min_ms,max_ms\n";

    auto run_scenario = [&](int thread_count, const char* scenario_name) {
        std::vector<double> timings(runs);
        for (int run = 0; run < runs; ++run) {
            std::vector<PrimeTask> tasks(thread_count);
            std::vector<int*> results(thread_count);
            std::vector<void*> handles;
            handles.reserve(thread_count);

            for (int t = 0; t < thread_count; ++t) {
                tasks[t].result = new int;
                results[t] = tasks[t].result;
                handles.push_back(cb.create_thread([](void* arg) {
                    prime_worker(static_cast<PrimeTask*>(arg));
                }, &tasks[t]));
            }

            double start = cb.now_ms();
            for (auto h : handles) {
                cb.join_thread(h);
            }
            timings[run] = cb.now_ms() - start;

            for (int* r : results) {
                delete r;
            }
        }

        double mn = timings[0], mx = timings[0];
        auto sorted = timings;
        std::sort(sorted.begin(), sorted.end());
        double median = sorted[sorted.size() / 2];
        std::cout << scenario_name << "," << thread_count << "," << median << "," << mn << "," << mx << "\n";
        std::cout << "  " << scenario_name << ": " << thread_count << " threads, median=" << median << "ms\n";
    };

    // 场景 A：所有线程在"大核"（核心后一半）上
    if (half > 0) {
        run_scenario(half, "ALL_BIG");
    }

    // 场景 B：所有线程在"小核"（核心前一半）上
    if (half > 0) {
        run_scenario(half, "ALL_LITTLE");
    }

    // 场景 C：混合 — 前一半重负载，后一半轻负载
    {
        int total_n = core_count;
        std::vector<double> timings(runs);
        for (int run = 0; run < runs; ++run) {
            std::vector<PrimeTask> tasks(total_n);
            std::vector<int*> results(total_n);
            std::vector<void*> handles;
            handles.reserve(total_n);

            // 前一半：重工作负载
            int chunk_heavy = upper_bound / (total_n / 2);
            for (int t = 0; t < total_n / 2; ++t) {
                tasks[t].lo = t * chunk_heavy;
                tasks[t].hi = (t + 1) * chunk_heavy - 1;
                if (t == total_n / 2 - 1) tasks[t].hi = upper_bound - 1;
                tasks[t].result = new int;
                results[t] = tasks[t].result;
                handles.push_back(cb.create_thread([](void* arg) {
                    prime_worker(static_cast<PrimeTask*>(arg));
                }, &tasks[t]));
            }
            // 后一半：轻工作负载
            for (int t = total_n / 2; t < total_n; ++t) {
                tasks[t].lo = 0;
                tasks[t].hi = 1000;
                tasks[t].result = new int;
                results[t] = tasks[t].result;
                handles.push_back(cb.create_thread([](void* arg) {
                    prime_worker(static_cast<PrimeTask*>(arg));
                }, &tasks[t]));
            }

            double start = cb.now_ms();
            for (auto h : handles) {
                cb.join_thread(h);
            }
            timings[run] = cb.now_ms() - start;

            for (int* r : results) {
                delete r;
            }
        }

        double mn = timings[0], mx = timings[0];
        auto sorted = timings;
        std::sort(sorted.begin(), sorted.end());
        double median = sorted[sorted.size() / 2];
        std::cout << "HYBRID," << total_n << "," << median << "," << mn << "," << mx << "\n";
        std::cout << "  HYBRID: " << total_n << " threads, median=" << median << "ms\n";
    }
}