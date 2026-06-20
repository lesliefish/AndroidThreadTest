// JNI bridge for Android APK
// Exports native methods called by NativeBench.kt
// Captures benchmark output via temp file

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

// ─── Core count ───────────────────────────────────────────────────

JNIEXPORT jint JNICALL
Java_com_example_threadtest_NativeBench_getCoreCount(JNIEnv *env, jobject thiz) {
    return (jint)android_get_core_count();
}

// ─── Cluster map ──────────────────────────────────────────────────

JNIEXPORT intArray JNICALL
Java_com_example_threadtest_NativeBench_getClusterMap(JNIEnv *env, jobject thiz) {
    int count;
    int* clusters = android_get_cluster_map(&count);
    jintArray arr = (*env)->NewIntArray(env, count);
    if (!arr) { free(clusters); return NULL; }
    (*env)->SetIntArrayRegion(env, arr, 0, count, (jint*)clusters);
    free(clusters);
    return arr;
}

// ─── Core binding ─────────────────────────────────────────────────

JNIEXPORT void JNICALL
Java_com_example_threadtest_NativeBench_bindToCore(JNIEnv *env, jobject thiz, jint coreId) {
    android_thread_bind_cpu((int)coreId);
}

// ─── Run all experiments (capture via temp file) ──────────────────

JNIEXPORT jstring JNICALL
Java_com_example_threadtest_NativeBench_runAllBenchmarks(JNIEnv *env, jobject thiz,
                                                          jint upperBound, jint runs) {
    // Use a temp file to capture benchmark output
    const char* tmpfile = "/data/local/tmp/thread_bench_out.txt";
    FILE* fp = fopen(tmpfile, "w");
    if (!fp) {
        return (*env)->NewStringUTF(env, "Error: cannot create temp file\n");
    }

    // Redirect stdout to temp file
    int saved_stdout = dup(STDOUT_FILENO);
    dup2(fileno(fp), STDOUT_FILENO);
    fclose(fp); // close our fd, the dup already points to the file

    // Run experiments — all printf goes to the temp file via stdout redirect
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
    fflush(stdout);

    // Restore stdout
    dup2(saved_stdout, STDOUT_FILENO);
    close(saved_stdout);

    // Read the temp file
    fp = fopen(tmpfile, "r");
    if (!fp) {
        unlink(tmpfile);
        return (*env)->NewStringUTF(env, "Error: cannot read temp file\n");
    }

    fseek(fp, 0, SEEK_END);
    long fsize = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    char* output = (char*)malloc((size_t)fsize + 1);
    if (!output) {
        fclose(fp);
        unlink(tmpfile);
        return (*env)->NewStringUTF(env, "Error: out of memory\n");
    }

    size_t read_len = fread(output, 1, (size_t)fsize, fp);
    output[read_len] = '\0';
    fclose(fp);
    unlink(tmpfile); // clean up

    jstring result = (*env)->NewStringUTF(env, output);
    free(output);
    return result;
}

// ─── JNI entry point ─────────────────────────────────────────────

JNIEXPORT jint JNICALL
JNI_OnLoad(JavaVM *vm, void *reserved) {
    (void)reserved;
    LOGI("JNI_OnLoad: ThreadBenchmark native library loaded");
    return JNI_VERSION_1_6;
}
