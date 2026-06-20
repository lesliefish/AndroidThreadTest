package com.example.threadtest

/**
 * JNI bridge to native benchmark code.
 * All benchmark logic runs in native C code; this class only provides
 * device info and triggers benchmark execution.
 */
object NativeBench {
    init {
        System.loadLibrary("threadtest_jni")
    }

    external fun getCoreCount(): Int
    external fun getClusterMap(): IntArray
    external fun bindToCore(coreId: Int)

    /**
     * Run all 3 benchmark experiments.
     * @param upperBound prime counting upper limit
     * @param runs per-config iteration count
     * @return CSV-formatted benchmark output
     */
    external fun runAllBenchmarks(upperBound: Int, runs: Int): String
}
