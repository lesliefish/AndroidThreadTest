// Windows 原生基准测试运行器 (C++11)
// 使用共享的 bench_core 实现跨平台基准测试逻辑。

#include "../shared/bench_core.h"
#include <windows.h>
#include <iostream>
#include <thread>
#include <chrono>
#include <vector>
#include <functional>
#include <algorithm>
#include <cstdlib>
#include <memory>

// RAII CPU 核心绑定器
class CpuBinder {
public:
    explicit CpuBinder(int core_id) : original_mask_(0), core_id_(core_id) {
        DWORD_PTR mask = static_cast<DWORD_PTR>(1ULL) << core_id;
        original_mask_ = SetThreadAffinityMask(GetCurrentThread(), mask);
    }
    ~CpuBinder() {
        if (original_mask_ != 0) {
            SetThreadAffinityMask(GetCurrentThread(), original_mask_);
        }
    }
    CpuBinder(const CpuBinder&) = delete;
    CpuBinder& operator=(const CpuBinder&) = delete;
private:
    DWORD_PTR original_mask_;
    int core_id_;
};

int get_core_count() {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return static_cast<int>(si.dwNumberOfProcessors);
}

std::vector<int> get_cluster_map() {
    int count = get_core_count();
    std::vector<int> clusters(count);
    for (int i = 0; i < count; ++i) {
        clusters[i] = (i < count / 2) ? 0 : 1;
    }
    return clusters;
}

double now_ms() {
    auto epoch = std::chrono::high_resolution_clock::now().time_since_epoch();
    return std::chrono::duration<double, std::milli>(epoch).count();
}

// 线程包装器
struct ThreadWrapper {
    std::function<void(void*)> func;
    void* arg;
};

int main(int argc, char* argv[]) {
    const char* platform = "windows";
    int upper_bound = 1000000;
    int runs = 10;

    if (argc > 1) platform = argv[1];
    if (argc > 2) upper_bound = atoi(argv[2]);
    if (argc > 3) runs = atoi(argv[3]);

    std::cout << "Platform: " << platform << "\n";
    std::cout << "Prime upper bound: " << upper_bound << "\n";
    std::cout << "Runs per config: " << runs << "\n";
    std::cout << "Core count: " << get_core_count() << "\n\n";

    BenchCallbacks cb;
    cb.get_core_count = []() { return get_core_count(); };
    cb.get_cluster_map = []() { return get_cluster_map(); };
    cb.now_ms = []() { return now_ms(); };
    cb.bind_cpu = [](int core_id) {
        CpuBinder binder(core_id);
    };

    // 使用 std::thread 实现线程创建和等待
    std::vector<std::unique_ptr<std::thread>> threads;
    std::vector<ThreadWrapper*> wrappers;
    
    cb.create_thread = [&wrappers](std::function<void(void*)> func, void* arg) -> void* {
        ThreadWrapper* tw = new ThreadWrapper{func, arg};
        wrappers.push_back(tw);
        return static_cast<void*>(tw);
    };

    cb.join_thread = [&wrappers, &threads](void*) {
        // 等待所有线程完成
        for (auto& t : threads) {
            if (t->joinable()) {
                t->join();
            }
        }
        threads.clear();
        
        // 执行所有包装的函数
        for (auto* tw : wrappers) {
            if (tw) {
                tw->func(tw->arg);
                delete tw;
            }
        }
        wrappers.clear();
    };

    std::cout << "=== Experiment 1: Prime Counting (Single vs Multi-thread) ===\n";
    run_prime_experiment(cb, platform, upper_bound, runs);

    std::cout << "\n=== Experiment 2: Big.LITTLE Simulation ===\n";
    run_big_little_test(cb, platform, upper_bound, runs);

    std::cout << "\n=== Experiment 3: Hybrid Scheduling ===\n";
    run_hybrid_test(cb, platform, upper_bound, runs);

    return 0;
}