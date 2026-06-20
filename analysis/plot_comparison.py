#!/usr/bin/env python3
"""
plot_comparison.py — Generate comparison charts from benchmark CSV results.

Usage:
    python plot_comparison.py <android_csv> <windows_csv> [--output-dir ./figures]

Produces:
    fig_1_speedup.png     — Speedup curve (Android pthread vs Windows CreateThread)
    fig_2_throughput.png  — Throughput comparison bar chart
    fig_3_big_little.png  — Big.LITTLE core comparison
"""

import sys
import os
import argparse
import pandas as pd
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
import matplotlib.gridspec as gridspec
import numpy as np

plt.rcParams['font.family'] = 'sans-serif'
plt.rcParams['axes.unicode_minus'] = False


def load_csv(path):
    """Load benchmark CSV, return DataFrame."""
    df = pd.read_csv(path)
    # Clean up: ensure numeric types
    for col in ['num_threads', 'median_ms', 'min_ms', 'max_ms', 'avg_ms',
                'p95_ms', 'p99_ms', 'throughput_ops', 'speedup']:
        if col in df.columns:
            df[col] = pd.to_numeric(df[col], errors='coerce')
    return df


def plot_speedup(android_df, windows_df, output_path):
    """Figure 1: Speedup curve comparing Android pthread vs Windows CreateThread."""
    fig, ax = plt.subplots(figsize=(10, 6))

    # Android
    if android_df is not None and len(android_df) > 0:
        ax.plot(android_df['num_threads'], android_df['speedup'],
                'o-', linewidth=2, markersize=8, label='Android (pthread)',
                color='#2196F3')

    # Windows
    if windows_df is not None and len(windows_df) > 0:
        ax.plot(windows_df['num_threads'], windows_df['speedup'],
                's-', linewidth=2, markersize=8, label='Windows (CreateThread)',
                color='#FF5722')

    # Ideal speedup line
    max_threads = max(
        android_df['num_threads'].max() if android_df is not None else 0,
        windows_df['num_threads'].max() if windows_df is not None else 0
    )
    if max_threads > 0:
        ideal = pd.Series(range(1, int(max_threads) + 2))
        ax.plot(ideal, ideal, 'k--', alpha=0.3, linewidth=1, label='Ideal (linear)')

    ax.set_xlabel('Number of Threads', fontsize=12)
    ax.set_ylabel('Speedup (relative to 1 thread)', fontsize=12)
    ax.set_title('Thread Scaling: Android pthread vs Windows CreateThread', fontsize=14, fontweight='bold')
    ax.legend(fontsize=11)
    ax.grid(True, alpha=0.3)
    ax.set_xlim(0.5, max_threads + 1.5)

    plt.tight_layout()
    plt.savefig(output_path, dpi=150, bbox_inches='tight')
    plt.close()
    print(f"  Saved: {output_path}")


def plot_throughput(android_df, windows_df, output_path):
    """Figure 2: Throughput comparison bar chart."""
    fig, ax = plt.subplots(figsize=(10, 6))

    if android_df is None or len(android_df) == 0:
        return

    x = np.arange(len(android_df))
    width = 0.35

    android_tp = android_df['throughput_ops'].values
    windows_tp = np.zeros(len(android_df))

    # Match Windows data by thread count
    if windows_df is not None:
        for i, nt in enumerate(android_df['num_threads']):
            match = windows_df[windows_df['num_threads'] == nt]
            if len(match) > 0:
                windows_tp[i] = match.iloc[0]['throughput_ops']

    bars1 = ax.bar(x - width/2, android_tp, width, label='Android (pthread)',
                   color='#2196F3', alpha=0.85)
    bars2 = ax.bar(x + width/2, windows_tp, width, label='Windows (CreateThread)',
                    color='#FF5722', alpha=0.85)

    ax.set_xlabel('Number of Threads', fontsize=12)
    ax.set_ylabel('Throughput (ops/sec)', fontsize=12)
    ax.set_title('Throughput Comparison: Prime Counting Benchmark', fontsize=14, fontweight='bold')
    ax.set_xticks(x)
    ax.set_xticklabels(android_df['num_threads'].astype(int).values)
    ax.legend(fontsize=11)
    ax.grid(True, axis='y', alpha=0.3)

    # Add value labels on bars
    for bar in bars1:
        h = bar.get_height()
        if h > 0:
            ax.annotate(f'{h:.0f}', xy=(bar.get_x() + bar.get_width()/2, h),
                       xytext=(0, 3), textcoords="offset points", ha='center', fontsize=8)
    for bar in bars2:
        h = bar.get_height()
        if h > 0:
            ax.annotate(f'{h:.0f}', xy=(bar.get_x() + bar.get_width()/2, h),
                       xytext=(0, 3), textcoords="offset points", ha='center', fontsize=8)

    plt.tight_layout()
    plt.savefig(output_path, dpi=150, bbox_inches='tight')
    plt.close()
    print(f"  Saved: {output_path}")


def plot_big_little(big_little_data, output_path):
    """Figure 3: Big.LITTLE core comparison radar/bar chart."""
    fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(14, 6))

    clusters = big_little_data.get('clusters', [])
    if not clusters:
        print("  No Big.LITTLE data to plot.")
        return

    little_data = big_little_data.get('little', {})
    big_data = big_little_data.get('big', {})

    if not little_data or not big_data:
        print("  Missing little/big data for Big.LITTLE chart.")
        return

    # Bar chart: median time
    categories = ['Median Time (ms)', 'Min Time (ms)', 'Max Time (ms)']
    little_values = [
        little_data.get('median_ms', 0),
        little_data.get('min_ms', 0),
        little_data.get('max_ms', 0)
    ]
    big_values = [
        big_data.get('median_ms', 0),
        big_data.get('min_ms', 0),
        big_data.get('max_ms', 0)
    ]

    x = np.arange(len(categories))
    width = 0.35

    ax1.bar(x - width/2, little_values, width, label='Little (E-core)',
            color='#4CAF50', alpha=0.85)
    ax1.bar(x + width/2, big_values, width, label='Big (P-core)',
            color='#FF9800', alpha=0.85)
    ax1.set_ylabel('Time (ms)', fontsize=12)
    ax1.set_title('Big.LITTLE: Core Performance Comparison', fontsize=13, fontweight='bold')
    ax1.set_xticks(x)
    ax1.set_xticklabels(categories)
    ax1.legend(fontsize=11)
    ax1.grid(True, axis='y', alpha=0.3)

    # Radar chart: normalized performance
    if little_values and big_values:
        # Normalize to big-core = 1.0
        little_norm = [lv / bv if bv > 0 else 0 for lv, bv in zip(little_values, big_values)]
        little_norm.append(little_values[0] / big_values[0] if big_values[0] > 0 else 0)

        angles = np.linspace(0, 2 * np.pi, len(categories) + 1, endpoint=True)
        little_vals = little_norm + [little_norm[0]]

        ax2.plot(angles, little_vals, 'o-', linewidth=2, color='#4CAF50',
                 label='Little / Big ratio')
        ax2.fill(angles, little_vals, alpha=0.15, color='#4CAF50')
        ax2.set_xticks(angles[:-1])
        ax2.set_xticklabels(categories)
        ax2.set_ylim(0, max(max(little_norm) * 1.2, 1.5))
        ax2.set_title('Little-core Efficiency vs Big-core', fontsize=13, fontweight='bold')
        ax2.legend(fontsize=11)
        ax2.grid(True, alpha=0.3)
        ax2.set_ylabel('Time ratio (higher = slower)', fontsize=10)

    plt.tight_layout()
    plt.savefig(output_path, dpi=150, bbox_inches='tight')
    plt.close()
    print(f"  Saved: {output_path}")


def plot_timing_box(android_df, windows_df, output_path):
    """Box plot showing timing distribution per thread count."""
    fig, ax = plt.subplots(figsize=(10, 6))

    if android_df is None or len(android_df) == 0:
        return

    x = np.arange(len(android_df))
    width = 0.35

    # Box-like visualization using min/max/median
    android_medians = android_df['median_ms'].values
    android_mins = android_df['min_ms'].values
    android_maxs = android_df['max_ms'].values

    windows_medians = np.zeros(len(android_df))
    windows_mins = np.zeros(len(android_df))
    windows_maxs = np.zeros(len(android_df))

    if windows_df is not None:
        for i, nt in enumerate(android_df['num_threads']):
            match = windows_df[windows_df['num_threads'] == nt]
            if len(match) > 0:
                windows_medians[i] = match.iloc[0]['median_ms']
                windows_mins[i] = match.iloc[0]['min_ms']
                windows_maxs[i] = match.iloc[0]['max_ms']

    # Error bars as proxy for box plot
    ax.errorbar(x - width/2, android_medians,
                yerr=[android_medians - android_mins, android_maxs - android_medians],
                fmt='o', capsize=4, color='#2196F3', label='Android',
                elinewidth=2, markersize=8)
    ax.errorbar(x + width/2, windows_medians,
                yerr=[windows_medians - windows_mins, windows_maxs - windows_medians],
                fmt='s', capsize=4, color='#FF5722', label='Windows',
                elinewidth=2, markersize=8)

    ax.set_xlabel('Number of Threads', fontsize=12)
    ax.set_ylabel('Execution Time (ms)', fontsize=12)
    ax.set_title('Timing Distribution: Min/Median/Max', fontsize=14, fontweight='bold')
    ax.set_xticks(x)
    ax.set_xticklabels(android_df['num_threads'].astype(int).values)
    ax.legend(fontsize=11)
    ax.grid(True, axis='y', alpha=0.3)

    plt.tight_layout()
    plt.savefig(output_path, dpi=150, bbox_inches='tight')
    plt.close()
    print(f"  Saved: {output_path}")


def main():
    parser = argparse.ArgumentParser(description='Plot benchmark comparison charts')
    parser.add_argument('android_csv', help='Android benchmark CSV file')
    parser.add_argument('windows_csv', help='Windows benchmark CSV file')
    parser.add_argument('--big-little-csv', help='Big.LITTLE CSV (optional)')
    parser.add_argument('--output-dir', default='./figures', help='Output directory')
    args = parser.parse_args()

    os.makedirs(args.output_dir, exist_ok=True)

    print(f"Loading Android results: {args.android_csv}")
    android_df = load_csv(args.android_csv)
    print(f"  Rows: {len(android_df)}, Columns: {list(android_df.columns)}")

    print(f"Loading Windows results: {args.windows_csv}")
    windows_df = load_csv(args.windows_csv)
    print(f"  Rows: {len(windows_df)}, Columns: {list(windows_df.columns)}")

    big_little_data = {}
    if args.big_little_csv:
        print(f"Loading Big.LITTLE results: {args.big_little_csv}")
        bl_df = load_csv(args.big_little_csv)
        for _, row in bl_df.iterrows():
            cluster = row.get('cluster', '')
            if 'little' in str(cluster).lower() or 'e-core' in str(cluster).lower():
                big_little_data['little'] = row.to_dict()
            elif 'big' in str(cluster).lower() or 'p-core' in str(cluster).lower():
                big_little_data['big'] = row.to_dict()

    print("\nGenerating charts...")
    plot_speedup(android_df, windows_df,
                 os.path.join(args.output_dir, 'fig_1_speedup.png'))
    plot_throughput(android_df, windows_df,
                    os.path.join(args.output_dir, 'fig_2_throughput.png'))
    plot_timing_box(android_df, windows_df,
                    os.path.join(args.output_dir, 'fig_3_timing_box.png'))
    plot_big_little(big_little_data,
                    os.path.join(args.output_dir, 'fig_4_big_little.png'))

    print("\nDone! Charts saved to", args.output_dir)


if __name__ == '__main__':
    main()
