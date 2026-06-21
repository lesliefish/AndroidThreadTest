package com.example.threadtest

/**
 * NativeBench — 原生基准测试代码的 JNI 桥接层。
 *
 * 这是唯一一个与原生代码接触的 Kotlin/Java 类。
 * 所有基准测试逻辑（线程创建、素数计数、统计计算）
 * 完全在 C 代码中运行。此类仅提供设备信息和触发执行。
 *
 * 架构：
 *   NativeBench.kt  →  libthreadtest_jni.so  →  native_bench.c（实验逻辑）
 *                      ↑ JNI 导出方法
 *                      ↑ 调用共享的实验函数
 *
 * 为什么使用 `object`（单例）而非 `class`？
 *   - 不需要实例状态；所有方法都是 external/static。
 *   - Kotlin 对象是惰性初始化的，所以 System.loadLibrary
 *     只在首次访问方法时调用一次。
 *
 * 原生库加载：
 *   System.loadLibrary("threadtest_jni") 从应用的 native 库路径
 *   加载 libthreadtest_jni.so（在 build.gradle 中通过 jniLibs.srcDirs 设置）。
 *   .so 由 CMake 从 jni_bridge.c 构建。
 */
object NativeBench {
    init {
        // 首次访问时加载原生共享库。
        // 这会触发 jni_bridge.c 中的 JNI_OnLoad。
        System.loadLibrary("threadtest_jni")
    }

    /**
     * 获取在线 CPU 核心数量。
     * 映射到 native_bench.c 中的 android_get_core_count()。
     */
    external fun getCoreCount(): Int

    /**
     * 获取 big.LITTLE 核心集群映射。
     * 返回 int[]，其中索引 i = 核心 i 的集群 ID。
     * 集群 0 = 小核（E-core），集群 1 = 大核（P-core）。
     * 映射到 native_bench.c 中的 android_get_cluster_map()。
     */
    external fun getClusterMap(): IntArray

    /**
     * 将调用线程绑定到指定 CPU 核心。
     * 适用于将线程 pinned 到大核以获得最大性能的场景。
     * 映射到 native_bench.c 中的 android_thread_bind_cpu()。
     *
     * @param coreId 从零开始的核心索引（0 到 coreCount-1）
     */
    external fun bindToCore(coreId: Int)

    /**
     * 运行全部 3 个基准测试实验并返回 CSV 输出。
     *
     * 这是从 DashboardActivity 调用的主入口。
     * 触发：
     *   实验 1：1/2/4/6/8/12/16 线程的素数计数
     *   实验 2：Big.LITTLE 核心分离测试
     *   实验 3：混合调度（全大核 / 全小核 / 混合）
     *
     * 原生层通过 stdout 重定向到临时文件捕获所有 printf 输出，
     * 然后读回并以 String 形式返回。
     *
     * @param upperBound 统计 [0, upperBound) 范围内的素数个数
     * @param runs 每配置的迭代次数
     * @return CSV 格式的基准测试输出（可由 plot_comparison.py 解析）
     */
    external fun runAllBenchmarks(upperBound: Int, runs: Int): String
}
