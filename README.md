# Android vs Windows 线程执行差异 — 实验项目

## 概述

通过同一套底层 C 代码，在 Android 和 Windows 两个平台上执行线程性能基准测试，对比两个操作系统内核调度层面的差异。

**核心原则**：Android（pthread）和 Windows（CreateThread）使用相同的素数计算、矩阵乘法算法，唯一的差异在于线程 API → 内核调度器（Linux CFS vs NT Scheduler）。

## 项目结构

```
AndroidThreadTest/
├── CMakeLists.txt              # 顶层 CMake：统一管理 Windows/Android 子项目
├── shared/                     # 共享头文件（thread_ops 接口声明）
│   └── thread_ops.h            # 线程操作 C ABI 接口
│
├── android/                    # Android NDK 测试
│   ├── app/
│   │   ├── CMakeLists.txt      # NDK CMake 配置（Android Studio 使用）
│   │   └── src/main/
│   │       ├── jni/
│   │       │   └── native_bench.c   # 入口：pthread + sched_setaffinity
│   │       ├── java/com/example/threadtest/
│   │       │   ├── DashboardActivity.kt  # UI 控制面板
│   │       │   └── NativeBench.kt      # JNI 桥接
│   │       └── AndroidManifest.xml
│   └── build.gradle            # Gradle 配置
│
├── windows/                    # Windows 测试程序
│   ├── CMakeLists.txt          # CMake 配置
│   └── src/
│       └── native_bench.c      # 入口：CreateThread + SetThreadAffinityMask
│
├── analysis/                   # 结果分析
│   ├── run_all.sh              # 一键跑两端实验
│   └── plot_comparison.py      # matplotlib 生成对比图
│
└── README.md
```

## 实验设计

### 实验 1：单线程 vs 多线程 — 性能对比

两个平台用相同代码跑 1/2/4/6/8/12 线程，素数计算基准：
- **加速比**：`Speedup = T(1线程) / T(N线程)`
- **吞吐量**：ops/sec
- **延迟分布**：min/median/max/p95/p99

### 实验 2：Android 大小核分离测试

通过 `sched_setaffinity` 强制绑定线程到 E-core 或 P-core：
- 小核单独跑 vs 大核单独跑
- 量化两类核心的绝对性能差异

### 实验 3：Android 大小核混合调度

- 全大核 vs 全小核 vs 混部（计算→大核，轻量→小核）
- 验证"任务分类 + 核心亲和"调度策略

## 构建与运行

### Windows（已验证可编译）

```bash
# 方法 1：直接在 windows/ 下构建
cd windows
mkdir build && cd build
cmake .. -G "Visual Studio 18 2026" -A x64
cmake --build . --config Release
# 运行
./Release/threadtest.exe windows 1000000 10

# 方法 2：通过顶层 CMake
cd ..
mkdir ../build && cd ../build
cmake .. -G "Visual Studio 18 2026" -A x64 -DBUILD_ANDROID=OFF
cmake --build . --config Release
# 产物在 build/windows/Release/threadtest.exe
```

### Android（需 NDK）

```bash
# 在 Android Studio 中打开 android/ 目录
# 1. 安装 Android NDK r25+
# 2. File → Open → 选择 android/ 目录
# 3. Sync Gradle
# 4. Build → Build Bundle(s) / APK(s)
# 5. 安装到设备运行
```

### 分析

```bash
# 两端都跑完后，用 Python 出图
pip install pandas matplotlib numpy
python analysis/plot_comparison.py <android_csv> <windows_csv> --output-dir ./figures
```

## 控制变量

1. Android 端固定 CPU frequency governor 为 `performance`
2. Windows 端设置为最高性能电源计划
3. 实验前设备预热 2 分钟
4. 每个测试点至少 10 次采样，取中位数

## 输出格式

CSV 到 stdout，列：
```
experiment,platform,num_threads,median_ms,min_ms,max_ms,avg_ms,p95_ms,p99_ms,throughput_ops,speedup
```

## 依赖

- **Windows**: CMake 3.15+, Visual Studio 2022/2026
- **Android**: Android NDK r25+, CMake 3.18+, Python 3.8+ (分析)
- **分析**: pandas, matplotlib, numpy
