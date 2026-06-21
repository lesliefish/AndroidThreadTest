// JNI 桥接层 — Android 应用的 native 库 (C++11)
// 导出供 Kotlin/Java 通过 JNI 调用的 C 函数。

#include <jni.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <android/log.h>
#include <sys/time.h>
#include <fstream>
#include <sstream>
#include <iostream>
#include <vector>
#include <memory>

#define LOG_TAG "ThreadBenchmark"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

// 声明 bench_core 中的函数
struct PrimeTask;
bool is_prime(int n);
struct BenchCallbacks;
void run_prime_experiment(const BenchCallbacks& cb,
                          const std::string& platform,
                          int upper_bound,
                          int runs);
void run_big_little_test(const BenchCallbacks& cb,
                         const std::string& platform,
                         int upper_bound,
                         int runs);
void run_hybrid_test(const BenchCallbacks& cb,
                     const std::string& platform,
                     int upper_bound,
                     int runs);

// Android 平台函数声明
extern "C" {
    int android_get_core_count(void);
    int* android_get_cluster_map(int* out_count);
}

// ─── 平台回调实现 ────────────────────────────────────────────────

static double android_now_ms() {
    timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec * 1000.0 + tv.tv_usec / 1000.0;
}

static void android_bind_cpu(int core_id) {
    // Android JNI 线程中不绑定CPU
    (void)core_id;
}

// ─── JNI 函数实现 ────────────────────────────────────────────────

JNIEXPORT jint JNICALL
Java_com_example_threadtest_NativeBench_getCoreCount(JNIEnv *env, jobject thiz) {
    return static_cast<jint>(android_get_core_count());
}

JNIEXPORT jintArray JNICALL
Java_com_example_threadtest_NativeBench_getClusterMap(JNIEnv *env, jobject thiz) {
    int count;
    int* clusters = android_get_cluster_map(&count);
    
    jintArray arr = env->NewIntArray(count);
    if (!arr) {
        delete[] clusters;
        return NULL;
    }
    env->SetIntArrayRegion(arr, 0, count, reinterpret_cast<jint*>(clusters));
    delete[] clusters;
    return arr;
}

JNIEXPORT void JNICALL
Java_com_example_threadtest_NativeBench_bindToCore(JNIEnv *env, jobject thiz, jint coreId) {
    android_bind_cpu(static_cast<int>(coreId));
}

JNIEXPORT jstring JNICALL
Java_com_example_threadtest_NativeBench_runAllBenchmarks(JNIEnv *env, jobject thiz,
                                                          jint upperBound, jint runs) {
    const char* platform = "android";
    int ub = static_cast<int>(upperBound);
    int r = static_cast<int>(runs);
    
    // 重定向输出到字符串流
    std::ostringstream output;
    std::streambuf* old_cout_buf = std::cout.rdbuf(output.rdbuf());
    
    // 构建回调
    BenchCallbacks cb;
    cb.get_core_count = []() { return android_get_core_count(); };
    cb.get_cluster_map = []() {
        int count;
        int* clusters = android_get_cluster_map(&count);
        std::vector<int> v(clusters, clusters + count);
        delete[] clusters;
        return v;
    };
    cb.now_ms = []() { return android_now_ms(); };
    cb.bind_cpu = android_bind_cpu;
    cb.create_thread = [](std::function<void(void*)> func, void* arg) -> void* {
        // 简化的线程创建 - 直接调用
        func(arg);
        return nullptr;
    };
    cb.join_thread = [](void*) {
        // 无需操作
    };
    
    std::cout << "Platform: " << platform << "\n";
    std::cout << "Prime upper bound: " << ub << "\n";
    std::cout << "Runs per config: " << r << "\n";
    std::cout << "Core count: " << android_get_core_count() << "\n\n";
    
    std::cout << "=== Experiment 1: Prime Counting (Single vs Multi-thread) ===\n";
    run_prime_experiment(cb, platform, ub, r);
    
    std::cout << "\n=== Experiment 2: Big.LITTLE Simulation ===\n";
    run_big_little_test(cb, platform, ub, r);
    
    std::cout << "\n=== Experiment 3: Hybrid Scheduling ===\n";
    run_hybrid_test(cb, platform, ub, r);
    
    std::cout << "\n=== Done ===\n";
    
    // 恢复 stdout
    std::cout.rdbuf(old_cout_buf);
    
    std::string result = output.str();
    return env->NewStringUTF(result.c_str());
}

JNIEXPORT jint JNICALL
JNI_OnLoad(JavaVM *vm, void *reserved) {
    (void)reserved;
    LOGI("JNI_OnLoad: ThreadBenchmark native library loaded");
    return JNI_VERSION_1_6;
}