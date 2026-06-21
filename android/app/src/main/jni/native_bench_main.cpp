// Android 原生基准测试主程序 (C++11)
// 使用共享的 bench_core 实现跨平台基准测试逻辑。

#include "../../../../../../shared/bench_core.h"
#include <pthread.h>
#include <sched.h>
#include <unistd.h>
#include <sys/sysinfo.h>
#include <sys/time.h>
#include <errno.h>
#include <stdio.h>
#include <vector>
#include <functional>
#include <cstring>
#include <cstdlib>
#include <iostream>

// 线程包装器
struct ThreadWrapper {
    std::function<void(void*)> func;
    void* arg;
};

// Android 平台特定的回调实现
static int android_get_core_count_impl() {
    return static_cast<int>(sysconf(_SC_NPROCESSORS_ONLN));
}

static std::vector<int> android_get_cluster_map_impl() {
    int count = android_get_core_count_impl();
    std::vector<int> clusters(count);
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
            clusters[i] = (i < count / 2) ? 0 : 1;
        }
    }
    return clusters;
}

static void android_thread_bind_cpu_impl(int core_id) {
    cpu_set_t mask;
    CPU_ZERO(&mask);
    CPU_SET(core_id, &mask);
    if (sched_setaffinity(0, sizeof(mask), &mask) != 0) {
        fprintf(stderr, "Warning: sched_setaffinity(cpu%d) failed: %s\n",
                core_id, strerror(errno));
    }
}

static double android_now_ms() {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec * 1000.0 + tv.tv_usec / 1000.0;
}

// 线程创建和等待的包装
static void* android_thread_create_impl(std::function<void(void*)> func, void* arg) {
    ThreadWrapper* tw = new ThreadWrapper{func, arg};
    pthread_t* tid = new pthread_t();
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 256 * 1024);
    
    if (pthread_create(tid, &attr, [](void* param) -> void* {
        ThreadWrapper* tw = static_cast<ThreadWrapper*>(param);
        tw->func(tw->arg);
        return NULL;
    }, tw) != 0) {
        delete tw;
        delete tid;
        return NULL;
    }
    pthread_attr_destroy(&attr);
    return static_cast<void*>(tid);
}

static void android_thread_join_impl(void* handle) {
    if (!handle) return;
    pthread_t* tid = static_cast<pthread_t*>(handle);
    pthread_join(*tid, NULL);
    delete tid;
}

// 实验函数声明（来自 bench_core.cpp）
extern void run_prime_experiment(const BenchCallbacks& cb,
                                  const std::string& platform,
                                  int upper_bound,
                                  int runs);
extern void run_big_little_test(const BenchCallbacks& cb,
                                 const std::string& platform,
                                 int upper_bound,
                                 int runs);
extern void run_hybrid_test(const BenchCallbacks& cb,
                             const std::string& platform,
                             int upper_bound,
                             int runs);

extern "C" {

// ─── 线程操作（供 jni_bridge.c 调用）───────────────────────────────

void* android_thread_create(void (*func)(void*), void* arg) {
    ThreadWrapper* tw = new ThreadWrapper{[func](void* a) { func(a); }, arg};
    pthread_t* tid = new pthread_t();
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 256 * 1024);
    
    if (pthread_create(tid, &attr, [](void* param) -> void* {
        ThreadWrapper* tw = static_cast<ThreadWrapper*>(param);
        tw->func(tw->arg);
        return NULL;
    }, tw) != 0) {
        delete tw;
        delete tid;
        return NULL;
    }
    pthread_attr_destroy(&attr);
    return static_cast<void*>(tid);
}

void android_thread_join(void* handle) {
    if (!handle) return;
    pthread_t* tid = static_cast<pthread_t*>(handle);
    pthread_join(*tid, NULL);
    delete tid;
}

void android_thread_bind_cpu(int core_id) {
    android_thread_bind_cpu_impl(core_id);
}

int android_get_core_count(void) {
    return android_get_core_count_impl();
}

int* android_get_cluster_map(int* out_count) {
    std::vector<int> clusters = android_get_cluster_map_impl();
    int count = static_cast<int>(clusters.size());
    int* result = new int[count];
    std::copy(clusters.begin(), clusters.end(), result);
    *out_count = count;
    return result;
}

void* prime_worker(void* arg) {
    PrimeTask* task = static_cast<PrimeTask*>(arg);
    int count = 0;
    for (int i = task->lo; i <= task->hi; ++i) {
        if (is_prime(i)) ++count;
    }
    *(task->result) = count;
    return NULL;
}

} // extern "C"