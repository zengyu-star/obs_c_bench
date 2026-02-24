#!/usr/bin/env python3
# -*- coding: utf-8 -*-

import os
import glob
import pandas as pd

def merge_latest_task_logs():
    # Automatically find the latest task directory
    task_dirs = sorted(glob.glob("logs/task_*"))
    if not task_dirs:
        print("[-] No benchmark task log directories found.")
        return
    
    latest_dir = task_dirs[-1]
    csv_files = glob.glob(os.path.join(latest_dir, "detail_*.csv"))
    
    if not csv_files:
        print(f"[-] No detail files found in {latest_dir}. Please ensure EnableDetailLog=true is set.")
        return

    print(f"[+] Merging {len(csv_files)} split log files in {latest_dir}...")
    
    try:
        # Read and concatenate all worker thread CSVs
        df_list = [pd.read_csv(f) for f in csv_files]
        merged_df = pd.concat(df_list, ignore_index=True)
        
        # Sort by timestamp to restore the global timeline
        merged_df.sort_values(by="Timestamp(s)", inplace=True)
        
        # Save as the final merged detail.csv
        output_path = os.path.join(latest_dir, "detail.csv")
        merged_df.to_csv(output_path, index=False)
        
        print(f"[+] Merge complete! Total records: {len(merged_df)}")
        print(f"[+] Final file path: {output_path}")
        
        # Calculate high-level statistical metrics
        if not merged_df.empty:
            p99_latency = merged_df["Latency(ms)"].quantile(0.99)
            avg_latency = merged_df["Latency(ms)"].mean()
            print("-" * 40)
            print(f"Metrics Overview:")
            print(f" > Avg Latency: {avg_latency:.2f} ms")
            print(f" > P99 Latency: {p99_latency:.2f} ms")
            print("-" * 40)
            
    except Exception as e:
        print(f"[-] An error occurred during the merge process: {e}")

if __name__ == "__main__":
    merge_latest_task_logs()
