#!/usr/bin/env python3
# -*- coding: utf-8 -*-

import os
import glob
import pandas as pd
import numpy as np
import matplotlib.pyplot as plt

def generate_dashboard(csv_path):
    print(f"[+] Loading data: {csv_path} ...")
    try:
        df = pd.read_csv(csv_path)
    except Exception as e:
        print(f"[-] Failed to read CSV: {e}")
        return

    if df.empty:
        print("[-] Dataset is empty. Cannot generate charts.")
        return

    # [适配]: 检查列名，兼容旧版本的 StatusCode 或新版本的 HTTPCode
    status_col = 'HTTPCode' if 'HTTPCode' in df.columns else 'StatusCode'
    if status_col not in df.columns:
        print(f"[-] Critical Error: Required column '{status_col}' not found in CSV.")
        return

    # 1. Data Preprocessing
    # Calculate relative time (starting from 0 seconds)
    df['RelativeTime'] = df['Timestamp(s)'] - df['Timestamp(s)'].min()
    # Floor to nearest second for aggregation
    df['TimeSec'] = df['RelativeTime'].astype(int)

    # Downsampling protection to prevent OOM or freezing on massive datasets
    MAX_SCATTER_POINTS = 100000
    if len(df) > MAX_SCATTER_POINTS:
        print(f"[*] Dataset size reached {len(df)}. Triggering downsampling (max {MAX_SCATTER_POINTS} points).")
        scatter_df = df.sample(n=MAX_SCATTER_POINTS, random_state=42)
    else:
        scatter_df = df

    # 2. Initialize 2x2 Dashboard Figure
    fig, axs = plt.subplots(2, 2, figsize=(18, 10))
    fig.suptitle('OBS C Benchmark Performance Dashboard', fontsize=20, fontweight='bold')

    # ==========================================
    # Dimension 1: Latency Scatter & Trend (Top-Left)
    # ==========================================
    ax1 = axs[0, 0]
    ax1.scatter(scatter_df['RelativeTime'], scatter_df['Latency(ms)'], 
                alpha=0.3, s=2, color='#1f77b4', label='Request Latency')
    
    trend = df.groupby('TimeSec')['Latency(ms)'].mean()
    ax1.plot(trend.index, trend.values, color='red', linewidth=2, label='1s Avg Trend')
    
    ax1.set_title("Latency Scatter & 1s Avg Trend", fontsize=14)
    ax1.set_xlabel("Time (seconds)")
    ax1.set_ylabel("Latency (ms)")
    ax1.grid(True, linestyle='--', alpha=0.6)
    ax1.legend(loc='upper right')

    # ==========================================
    # Dimension 2: Latency CDF & P99 (Top-Right)
    # ==========================================
    ax2 = axs[0, 1]
    sorted_latency = np.sort(df['Latency(ms)'])
    p = 1. * np.arange(len(df)) / (len(df) - 1)
    
    ax2.plot(sorted_latency, p, color='#9467bd', linewidth=2)
    
    p90_val = np.percentile(sorted_latency, 90)
    p99_val = np.percentile(sorted_latency, 99)
    p999_val = np.percentile(sorted_latency, 99.9)

    ax2.axvline(x=p99_val, color='red', linestyle='--', label=f'P99: {p99_val:.2f} ms')
    ax2.axvline(x=p90_val, color='orange', linestyle=':', label=f'P90: {p90_val:.2f} ms')
    
    ax2.set_xlim(0, p999_val * 2 if p999_val > 0 else max(sorted_latency))
    ax2.set_title("Latency Cumulative Distribution (CDF)", fontsize=14)
    ax2.set_xlabel("Latency (ms)")
    ax2.set_ylabel("Cumulative Probability")
    ax2.grid(True, linestyle='--', alpha=0.6)
    ax2.legend(loc='lower right')

    # ==========================================
    # Dimension 3: Instant TPS & Bandwidth (Bottom-Left)
    # ==========================================
    ax3 = axs[1, 0]
    tps_series = df.groupby('TimeSec').size()
    ax3.plot(tps_series.index, tps_series.values, color='#2ca02c', linewidth=2, label='Instant TPS')
    ax3.fill_between(tps_series.index, tps_series.values, color='#2ca02c', alpha=0.2)
    ax3.set_xlabel("Time (seconds)")
    ax3.set_ylabel("Requests per Second (TPS)", color='#2ca02c')
    ax3.tick_params(axis='y', labelcolor='#2ca02c')

    ax3_bw = ax3.twinx()
    bw_series = df.groupby('TimeSec')['Bytes'].sum() / (1024 * 1024)
    ax3_bw.plot(bw_series.index, bw_series.values, color='#ff7f0e', linewidth=2, linestyle='-.', label='Bandwidth (MB/s)')
    ax3_bw.set_ylabel("Bandwidth (MB/s)", color='#ff7f0e')
    ax3_bw.tick_params(axis='y', labelcolor='#ff7f0e')

    ax3.set_title("Instant TPS & Bandwidth over Time", fontsize=14)
    ax3.grid(True, linestyle='--', alpha=0.6)
    
    lines_1, labels_1 = ax3.get_legend_handles_labels()
    lines_2, labels_2 = ax3_bw.get_legend_handles_labels()
    ax3.legend(lines_1 + lines_2, labels_1 + labels_2, loc='upper left')

    # ==========================================
    # Dimension 4: HTTP Status Code Distribution (Bottom-Right)
    # ==========================================
    ax4 = axs[1, 1]
    # [优化]: 使用重构后的状态码列
    status_counts = df[status_col].value_counts()
    
    # [优化]: 更加精准的颜色分配
    # 2xx 成功码设为绿色，其余（4xx, 5xx, 或 0代表的NetError）设为红色
    colors = ['#2ca02c' if str(code).startswith('2') else '#d62728' for code in status_counts.index]
    
    ax4.pie(status_counts.values, labels=[f"Code {code}" for code in status_counts.index], 
            autopct='%1.2f%%', startangle=140, colors=colors, 
            wedgeprops={'edgecolor': 'white', 'linewidth': 1})
    
    centre_circle = plt.Circle((0,0), 0.70, fc='white')
    ax4.add_artist(centre_circle)
    
    ax4.set_title(f"Response {status_col} Distribution", fontsize=14)

    # ==========================================
    # Layout and Save
    # ==========================================
    plt.tight_layout(rect=[0, 0, 1, 0.96])
    
    output_dir = os.path.dirname(csv_path)
    output_img = os.path.join(output_dir, "dashboard.png")
    
    plt.savefig(output_img, dpi=150, bbox_inches='tight')
    print(f"[+] Dashboard generated successfully: {output_img}")

def main():
    task_dirs = sorted(glob.glob("logs/task_*"))
    if not task_dirs:
        print("[-] No logs/task_* directories found.")
        return
    
    latest_dir = task_dirs[-1]
    csv_path = os.path.join(latest_dir, "detail.csv")
    
    if not os.path.exists(csv_path):
        print(f"[-] detail.csv not found in {latest_dir}.")
        print("    Please run merge_details.py first.")
        return
        
    generate_dashboard(csv_path)

if __name__ == "__main__":
    main()

