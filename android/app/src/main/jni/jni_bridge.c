// JNI 桥接层 — Android 应用的 native 库
//
// 功能：
//   导出供 Kotlin/Java 通过 JNI 调用的 C 函数，使 Android 应用能够
//   直接运行基准测试实验，而无需启动子进程。
//
// 架构：
//   - JNI 方法委托给 native_bench.c 中定义的函数
//     （通过 native_bench_api.h 公开）。
//   - 基准测试输出通过重定向 stdout 到临时文件来捕获，
//     然后读回并以 Java String 形式返回。
//
// 构建：
//   通过 CMake 编译为 libthreadtest_jni.so（参见 android/app/CMakeLists.txt）。
//   由 NativeBench.kt 通过 System.loadLibrary("threadtest_jni") 加载。

#include <jni.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sched.h>
#include <unistd.h>
#include <sys/sysinfo.h>
#include <sys/time.h>
#include <android/log.h>
#include "native_bench_api.h"

#define LOG_TAG "ThreadBenchmark"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

// ─── getCoreCount ─────────────────────────────────────────────────
// 从 Kotlin 的 NativeBench.getCoreCount() 调用。
// 返回设备上在线 CPU 核心的数量。
//
// JNI 命名规范：Java_<包名>_<类名>_<方法名>
//   包名: com.example.threadtest
//   类名: NativeBench
//   方法名: getCoreCount
JNIEXPORT jint JNICALL
Java_com_example_threadtest_NativeBench_getCoreCount(JNIEnv *env, jobject thiz) {
    return (jint)android_get_core_count();
}

// ─── getClusterMap ────────────────────────────────────────────────
// 从 Kotlin 的 NativeBench.getClusterMap() 调用。
// 返回一个 int[]，其中索引 i 对应核心 i 的集群 ID。
//
// 集群 ID 从 /sys/devices/system/cpu/cpu*/topology/cluster_id 读取。
// 如果 sysfs 条目不存在，所有核心默认为集群 0。
JNIEXPORT intArray JNICALL
Java_com_example_threadtest_NativeBench_getClusterMap(JNIEnv *env, jobject thiz) {
    int count;
    // 复用 native_bench.c 中的集群检测逻辑
    int* clusters = android_get_cluster_map(&count);

    // 分配一个 Java int[] 并拷贝数据
    jintArray arr = (*env)->NewIntArray(env, count);
    if (!arr) { free(clusters); return NULL; }
    (*env)->SetIntArrayRegion(env, arr, 0, count, (jint*)clusters);
    free(clusters);
    return arr;
}

// ─── bindToCore ───────────────────────────────────────────────────
// 从 Kotlin 的 NativeBench.bindToCore(coreId) 调用。
// 将调用线程绑定到指定的 CPU 核心。
//
// 用于细粒度的亲和性控制（例如，将特定线程绑定到大核以获得最大性能）。
JNIEXPORT void JNICALL
Java_com_example_threadtest_NativeBench_bindToCore(JNIEnv *env, jobject thiz, jint coreId) {
    android_thread_bind_cpu((int)coreId);
}

// ─── runAllBenchmarks ─────────────────────────────────────────────
// 从 Kotlin 的 NativeBench.runAllBenchmarks(upperBound, runs) 调用。
// 执行全部三个实验并以 String 形式返回完整的 CSV 输出。
//
// 输出捕获策略：
//   1. 在 /data/local/tmp/thread_bench_out.txt 打开临时文件
//   2. 使用 dup2() 将 stdout 重定向到临时文件
//   3. 调用三个实验函数 — 它们的 printf 输出现在进入临时文件
//   4. 恢复原始 stdout
//   5. 读取临时文件，将其内容作为 Java String 返回
//   6. 删除临时文件
//
// 为什么用临时文件而不是管道？
//   Android 的 JNI 环境不支持 fork()，因此无法使用 popen()。
//   dup2() 是最简单的方式，可以在不修改实验函数的情况下捕获所有 printf 输出。
JNIEXPORT jstring JNICALL
Java_com_example_threadtest_NativeBench_runAllBenchmarks(JNIEnv *env, jobject thiz,
                                                          jint upperBound, jint runs) {
    // 步骤 1：打开临时文件用于写入
    const char* tmpfile = "/data/local/tmp/thread_bench_out.txt";
    FILE* fp = fopen(tmpfile, "w");
    if (!fp) {
        return (*env)->NewStringUTF(env, "错误：无法创建临时文件\n");
    }

    // 步骤 2：保存原始 stdout 并重定向到临时文件
    int saved_stdout = dup(STDOUT_FILENO);
    dup2(fileno(fp), STDOUT_FILENO);
    fclose(fp);  // 我们的 FILE* 不再需要；dup2 已经接管了 fd

    // 步骤 3：运行所有三个实验 — 所有 printf 输出进入 tmpfile
    fprintf(stdout, "Platform: android\n");
    fprintf(stdout, "Prime upper bound: %d\n", (int)upperBound);
    fprintf(stdout, "Runs per config: %d\n", (int)runs);
    fprintf(stdout, "Core count: %d\n\n", android_get_core_count());

    fprintf(stdout, "=== Experiment 1: Prime Counting (Single vs Multi-thread) ===\n");
    run_prime_experiment("android", (int)upperBound, (int)runs);

    fprintf(stdout, "\n=== Experiment 2: Big.LITTLE Separation ===\n");
    run_big_little_test("android", (int)upperBound, (int)runs);

    fprintf(stdout, "\n=== Experiment 3: Hybrid Scheduling ===\n");
    run_hybrid_test("android", (int)upperBound, (int)runs);

    fprintf(stdout, "\n=== Done ===\n");
    fflush(stdout);  // 确保所有缓冲数据写入临时文件

    // 步骤 4：恢复原始 stdout
    dup2(saved_stdout, STDOUT_FILENO);
    close(saved_stdout);

    // 步骤 5：从临时文件读取捕获的输出
    fp = fopen(tmpfile, "r");
    if (!fp) {
        unlink(tmpfile);
        return (*env)->NewStringUTF(env, "错误：无法读取临时文件\n");
    }

    fseek(fp, 0, SEEK_END);
    long fsize = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    char* output = (char*)malloc((size_t)fsize + 1);
    if (!output) {
        fclose(fp);
        unlink(tmpfile);
        return (*env)->NewStringUTF(env, "错误：内存不足\n");
    }

    size_t read_len = fread(output, 1, (size_t)fsize, fp);
    output[read_len] = '\0';
    fclose(fp);
    unlink(tmpfile);  // 清理临时文件

    // 步骤 6：转换为 Java String 并返回
    jstring result = (*env)->NewStringUTF(env, output);
    free(output);
    return result;
}

// ─── JNI_OnLoad ───────────────────────────────────────────────────
// 当通过 System.loadLibrary() 加载 .so 时自动调用。
//
// 我们在 Android Logcat 中记录一条消息，让开发者可以验证
// native 库是否正确加载。返回 JNI_VERSION_1_6
// 告诉 VM 我们支持 1.6 版本的 JNI。
JNIEXPORT jint JNICALL
JNI_OnLoad(JavaVM *vm, void *reserved) {
    (void)reserved;
    LOGI("JNI_OnLoad: ThreadBenchmark native library loaded");
    return JNI_VERSION_1_6;
}
