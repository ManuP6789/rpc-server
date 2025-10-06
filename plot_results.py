#!/usr/bin/env python3
"""
Performance analysis and visualization for RPC load tests
"""

import pandas as pd
import matplotlib.pyplot as plt
import sys
import numpy as np

def plot_latency_curves(df, output_prefix='rpc_analysis'):
    """Generate load-latency curves"""
    fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(15, 6))
    
    # Average and P95 latency
    ax1.plot(df['RequestsPerSec'], df['AvgLatency'], 
             marker='o', linewidth=2, label='Average Latency')
    ax1.plot(df['RequestsPerSec'], df['P95'], 
             marker='s', linewidth=2, label='P95 Latency')
    ax1.plot(df['RequestsPerSec'], df['P99'], 
             marker='^', linewidth=2, label='P99 Latency')
    
    ax1.set_xlabel('Request Rate (req/s)', fontsize=12)
    ax1.set_ylabel('Latency (ms)', fontsize=12)
    ax1.set_title('Load vs. Latency', fontsize=14, fontweight='bold')
    ax1.legend()
    ax1.grid(True, alpha=0.3)
    ax1.set_xscale('log')
    
    # Throughput analysis
    ax2.plot(df['RequestsPerSec'], df['Throughput'], 
             marker='o', linewidth=2, label='Actual Throughput', color='green')
    ax2.plot(df['RequestsPerSec'], df['RequestsPerSec'], 
             linestyle='--', linewidth=1, label='Target Throughput', 
             color='gray', alpha=0.7)
    
    ax2.set_xlabel('Target Request Rate (req/s)', fontsize=12)
    ax2.set_ylabel('Actual Throughput (req/s)', fontsize=12)
    ax2.set_title('Throughput Saturation', fontsize=14, fontweight='bold')
    ax2.legend()
    ax2.grid(True, alpha=0.3)
    ax2.set_xscale('log')
    ax2.set_yscale('log')
    
    plt.tight_layout()
    plt.savefig(f'{output_prefix}_latency.png', dpi=300, bbox_inches='tight')
    print(f"Saved {output_prefix}_latency.png")
    plt.close()

def plot_percentile_distribution(df, output_prefix='rpc_analysis'):
    """Plot percentile distributions at different loads"""
    fig, ax = plt.subplots(figsize=(12, 6))
    
    # Select interesting load points
    load_points = [100, 500, 1000, 2000]
    available_loads = [l for l in load_points if l in df['RequestsPerSec'].values]
    
    x = np.arange(len(['P50', 'P95', 'P99', 'P999']))
    width = 0.15
    
    for i, load in enumerate(available_loads):
        row = df[df['RequestsPerSec'] == load].iloc[0]
        values = [row['P50'], row['P95'], row['P99'], row['P999']]
        ax.bar(x + i*width, values, width, label=f'{load} req/s')
    
    ax.set_ylabel('Latency (ms)', fontsize=12)
    ax.set_title('Latency Percentiles at Different Load Levels', 
                 fontsize=14, fontweight='bold')
    ax.set_xticks(x + width * (len(available_loads)-1) / 2)
    ax.set_xticklabels(['P50', 'P95', 'P99', 'P99.9'])
    ax.legend()
    ax.grid(True, alpha=0.3, axis='y')
    
    plt.tight_layout()
    plt.savefig(f'{output_prefix}_percentiles.png', dpi=300, bbox_inches='tight')
    print(f"Saved {output_prefix}_percentiles.png")
    plt.close()

def plot_success_rate(df, output_prefix='rpc_analysis'):
    """Plot success rate analysis"""
    fig, ax = plt.subplots(figsize=(10, 6))
    
    total_requests = df['SuccessCount'] + df['ErrorCount']
    success_rate = (df['SuccessCount'] / total_requests) * 100
    
    ax.plot(df['RequestsPerSec'], success_rate, 
            marker='o', linewidth=2, color='green')
    ax.axhline(y=100, color='gray', linestyle='--', alpha=0.5)
    ax.axhline(y=99, color='orange', linestyle='--', alpha=0.5, 
               label='99% threshold')
    
    ax.set_xlabel('Request Rate (req/s)', fontsize=12)
    ax.set_ylabel('Success Rate (%)', fontsize=12)
    ax.set_title('Request Success Rate vs. Load', 
                 fontsize=14, fontweight='bold')
    ax.legend()
    ax.grid(True, alpha=0.3)
    ax.set_xscale('log')
    ax.set_ylim([95, 101])
    
    plt.tight_layout()
    plt.savefig(f'{output_prefix}_success_rate.png', dpi=300, bbox_inches='tight')
    print(f"Saved {output_prefix}_success_rate.png")
    plt.close()

def generate_summary_stats(df):
    """Print summary statistics"""
    print("\n" + "="*60)
    print("RPC PERFORMANCE SUMMARY")
    print("="*60)
    
    print(f"\nLoad Range: {df['RequestsPerSec'].min():.0f} - "
          f"{df['RequestsPerSec'].max():.0f} req/s")
    
    # Find knee point (where latency starts increasing significantly)
    latency_increase = df['P95'].diff() / df['RequestsPerSec'].diff()
    knee_idx = latency_increase.idxmax()
    if pd.notna(knee_idx):
        knee_load = df.loc[knee_idx, 'RequestsPerSec']
        print(f"\nEstimated Saturation Point: ~{knee_load:.0f} req/s")
        print(f"  P95 Latency at saturation: {df.loc[knee_idx, 'P95']:.2f} ms")
    
    # Best performance (lowest load)
    best_idx = df['RequestsPerSec'].idxmin()
    print(f"\nBest Performance ({df.loc[best_idx, 'RequestsPerSec']:.0f} req/s):")
    print(f"  Average Latency: {df.loc[best_idx, 'AvgLatency']:.2f} ms")
    print(f"  P95 Latency: {df.loc[best_idx, 'P95']:.2f} ms")
    print(f"  P99 Latency: {df.loc[best_idx, 'P99']:.2f} ms")
    
    # Maximum throughput
    max_throughput_idx = df['Throughput'].idxmax()
    print(f"\nMaximum Throughput: {df.loc[max_throughput_idx, 'Throughput']:.0f} req/s")
    print(f"  Target Load: {df.loc[max_throughput_idx, 'RequestsPerSec']:.0f} req/s")
    print(f"  P95 Latency: {df.loc[max_throughput_idx, 'P95']:.2f} ms")
    
    # Error analysis
    total_errors = df['ErrorCount'].sum()
    total_success = df['SuccessCount'].sum()
    error_rate = (total_errors / (total_success + total_errors)) * 100
    print(f"\nOverall Statistics:")
    print(f"  Total Successful Requests: {total_success:,}")
    print(f"  Total Errors: {total_errors:,}")
    print(f"  Overall Error Rate: {error_rate:.4f}%")
    
    # Latency ranges
    print(f"\nLatency Ranges:")
    print(f"  Average: {df['AvgLatency'].min():.2f} - {df['AvgLatency'].max():.2f} ms")
    print(f"  P95: {df['P95'].min():.2f} - {df['P95'].max():.2f} ms")
    print(f"  P99: {df['P99'].min():.2f} - {df['P99'].max():.2f} ms")
    print(f"  P99.9: {df['P999'].min():.2f} - {df['P999'].max():.2f} ms")
    
    print("\n" + "="*60 + "\n")

def main():
    if len(sys.argv) < 2:
        print("Usage: python3 plot_results.py <results.csv> [output_prefix]")
        sys.exit(1)
    
    csv_file = sys.argv[1]
    output_prefix = sys.argv[2] if len(sys.argv) > 2 else 'rpc_analysis'
    
    try:
        df = pd.read_csv(csv_file)
        print(f"Loaded {len(df)} test results from {csv_file}")
        
        # Generate all plots
        plot_latency_curves(df, output_prefix)
        plot_percentile_distribution(df, output_prefix)
        plot_success_rate(df, output_prefix)
        
        # Print summary
        generate_summary_stats(df)
        
        print("\nAnalysis complete! Generated plots:")
        print(f"  - {output_prefix}_latency.png")
        print(f"  - {output_prefix}_percentiles.png")
        print(f"  - {output_prefix}_success_rate.png")
        
    except FileNotFoundError:
        print(f"Error: Could not find file '{csv_file}'")
        sys.exit(1)
    except Exception as e:
        print(f"Error: {e}")
        sys.exit(1)

if __name__ == "__main__":
    main()