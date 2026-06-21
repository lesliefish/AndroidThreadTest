package com.example.threadtest

import android.os.Bundle
import android.widget.*
import androidx.appcompat.app.AppCompatActivity
import java.io.File

/**
 * DashboardActivity — 基准测试的 UI 控制面板。
 *
 * 这是整个项目中唯一的 Java/Kotlin 层。所有基准测试逻辑都在
 * 原生 C 代码中运行（Android 端用 pthreads，Windows 端用 CreateThread）。
 *
 * 执行流程：
 *   1. 创建时：通过 JNI 读取 CPU 核心数并显示。
 *   2. 用户点击"Run All Benchmarks"：
 *      a. 禁用按钮，显示 Toast 提示。
 *      b. 在后台线程中（非 AsyncTask/协程 — 简单起见）调用
 *         NativeBench.runAllBenchmarks()，触发 JNI 桥接。
 *      c. JNI 层将 stdout 重定向到临时文件，运行全部 3 个实验，
 *         读取文件内容并以 Java String 形式返回。
 *      d. 在可滚动的 TextView 中显示结果并保存到文件。
 *   3. 重新启用按钮。
 *
 * 为什么不使用 ProcessBuilder 启动 native_bench 可执行文件？
 *   因为 APK 打包的是 .so 动态库，不是独立二进制文件。
 *   JNI 方式更干净 — 无需提取文件、无需处理权限、无需管理子进程。
 */
class DashboardActivity : AppCompatActivity() {

    // UI 组件
    private lateinit var outputText: TextView    // 显示基准测试结果
    private lateinit var runButton: Button       // 触发基准测试执行
    private lateinit var coreCountLabel: TextView // 显示设备核心数

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_dashboard)

        // 通过 ID 绑定 UI 视图
        outputText = findViewById(R.id.output_text)
        runButton = findViewById(R.id.run_button)
        coreCountLabel = findViewById(R.id.core_count_label)

        // 查询原生代码获取核心数并显示
        val coreCount = NativeBench.getCoreCount()
        coreCountLabel.text = "CPU Cores: $coreCount"

        // 按钮点击事件
        runButton.setOnClickListener {
            runButton.isEnabled = false
            outputText.text = "Running benchmarks...\n"
            Toast.makeText(this, "Running benchmarks...", Toast.LENGTH_SHORT).show()

            // 在后台线程运行基准测试（避免阻塞 UI 线程导致 ANR）
            Thread {
                try {
                    // 调用 JNI 运行全部 3 个实验。
                    // 参数：upper_bound=1000000, runs=5
                    // （移动端采样次数较少以加快反馈；桌面端使用 10 次）
                    val result = NativeBench.runAllBenchmarks(1000000, 5)
                    runOnUiThread {
                        outputText.text = result
                        // 将结果保存到内部存储以便后续分析
                        val csvFile = File(filesDir, "benchmark_results.csv")
                        csvFile.writeText(result)
                        Toast.makeText(
                            this,
                            "Saved to ${csvFile.absolutePath}",
                            Toast.LENGTH_LONG
                        ).show()
                    }
                } catch (e: Exception) {
                    runOnUiThread {
                        outputText.text = "Error: ${e.message}\n${e.stackTraceToString()}"
                        Toast.makeText(this, "Benchmark failed", Toast.LENGTH_SHORT).show()
                    }
                } finally {
                    runOnUiThread { runButton.isEnabled = true }
                }
            }.start()
        }
    }
}
