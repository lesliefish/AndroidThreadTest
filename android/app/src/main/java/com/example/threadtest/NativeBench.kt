package com.example.threadtest

/**
 * JNI bridge to native benchmark code.
 * All benchmark logic runs in native C code; this class only provides
 * device info (core count, cluster map) to the UI layer.
 */
object NativeBench {
    init {
        System.loadLibrary("threadtest_jni")
    }

    external fun getCoreCount(): Int
    external fun getClusterMap(): IntArray
    external fun bindToCore(coreId: Int)
}
