package com.example.threadtest

import android.os.Bundle
import android.widget.*
import androidx.appcompat.app.AppCompatActivity
import java.io.BufferedReader
import java.io.InputStreamReader

/**
 * DashboardActivity — UI control panel for running benchmarks.
 * Launches native benchmark executables via ProcessBuilder and displays results.
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

        // Show core count
        val coreCount = NativeBench.getCoreCount()
        coreCountLabel.text = "CPU Cores: $coreCount"

        runButton.setOnClickListener {
            runButton.isEnabled = false
            Toast.makeText(this, "Running benchmarks...", Toast.LENGTH_SHORT).show()
            Thread {
                runBenchmarks()
                runOnUiThread {
                    runButton.isEnabled = true
                }
            }.start()
        }
    }

    private fun runBenchmarks() {
        try {
            // Extract native binary from assets and run it
            val binaryPath = extractBinary("native_bench")
            val process = Runtime.getRuntime().exec arrayOf(binaryPath, "android", "1000000", "5")
            val reader = BufferedReader(InputStreamReader(process.inputStream))
            val errorReader = BufferedReader(InputStreamReader(process.errorStream))

            val output = StringBuilder()
            var line: String?
            while (reader.readLine().also { line = it } != null) {
                output.append(line).append('\n')
            }
            while (errorReader.readLine().also { line = it } != null) {
                output.append("[ERR] ").append(line).append('\n')
            }

            // Save CSV to file for analysis
            val csvFile = filesDir.resolve("benchmark_results.csv")
            csvFile.writeText(output.toString())

            // Display results
            runOnUiThread {
                outputText.text = output.toString()
                Toast.makeText(this, "Results saved to ${csvFile.absolutePath}", Toast.LENGTH_LONG).show()
            }
        } catch (e: Exception) {
            runOnUiThread {
                outputText.text = "Error: ${e.message}"
                Toast.makeText(this, "Benchmark failed: ${e.message}", Toast.LENGTH_LONG).show()
            }
        }
    }

    private fun extractBinary(name: String): String {
        val dest = filesDir.resolve(name)
        if (!dest.exists()) {
            assets.open(name).use { input ->
                dest.outputStream().use { output ->
                    input.copyTo(output)
                }
            }
            dest.setExecutable(true)
        }
        return dest.absolutePath
    }
}
