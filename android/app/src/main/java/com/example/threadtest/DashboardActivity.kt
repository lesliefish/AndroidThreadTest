package com.example.threadtest

import android.os.Bundle
import android.widget.*
import androidx.appcompat.app.AppCompatActivity
import java.io.File

/**
 * DashboardActivity — UI control panel for running benchmarks.
 * Calls native benchmark code via JNI (libthreadtest_jni.so).
 */
class DashboardActivity : AppCompatActivity() {

    private lateinit var outputText: TextView
    private lateinit var runButton: Button
    private lateinit var coreCountLabel: TextView

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_dashboard)

        outputText = findViewById(R.id.output_text)
        runButton = findViewById(R.id.run_button)
        coreCountLabel = findViewById(R.id.core_count_label)

        val coreCount = NativeBench.getCoreCount()
        coreCountLabel.text = "CPU Cores: $coreCount"

        runButton.setOnClickListener {
            runButton.isEnabled = false
            outputText.text = "Running benchmarks...\n"
            Toast.makeText(this, "Running benchmarks...", Toast.LENGTH_SHORT).show()

            Thread {
                try {
                    val result = NativeBench.runAllBenchmarks(1000000, 5)
                    runOnUiThread {
                        outputText.text = result
                        // Save CSV to file
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
