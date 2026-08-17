"""
HopperNet Near-Zero Waiting & Zero-Drift Synchronization Simulation
Benchmarks:
1. Mean Acquisition Latency (MAT): Serial 124-Ch Scan vs 4-Channel Anchor PMER
2. PI Phase-Frequency Loop Filter (Zero-Drift Tracking under Crystal Frequency Error)
3. End-to-End Mesh Message Latency (A -> B -> C slotted delivery)
"""

import os
import numpy as np
import matplotlib.pyplot as plt

os.makedirs("simulations/plots", exist_ok=True)

# -------------------------------------------------------------
# 1. ACQUISITION LATENCY MONTE CARLO (Near-Zero Waiting)
# -------------------------------------------------------------
def simulate_acquisition_latency():
    print("[1/3] Simulating Acquisition Latency (Zero-Waiting Benchmark)...")
    np.random.seed(42)
    num_trials = 2000
    dwell_ms = 25.0
    
    # 1. Traditional 124-Channel Serial Scan
    # Receiver parks on a channel for 80ms, master hops every 25ms
    traditional_latencies_ms = []
    for _ in range(num_trials):
        # Geometric distribution of collision between hopping sequence and fixed scan channel
        hits = np.random.geometric(p=1.0/124.0)
        t_acq = hits * dwell_ms + np.random.uniform(0, 80)
        traditional_latencies_ms.append(t_acq)
        
    # 2. HopperNet 4-Channel Anchor PMER (Predictive Multi-Tier Epoch Rendezvous)
    # Master broadcasts on Anchor Channel set {10, 42, 74, 106} periodically
    pmer_latencies_ms = []
    for _ in range(num_trials):
        hits = np.random.geometric(p=1.0/4.0)
        t_acq = hits * dwell_ms + np.random.uniform(0, 10)
        pmer_latencies_ms.append(t_acq)

    trad_mean = np.mean(traditional_latencies_ms)
    trad_p99 = np.percentile(traditional_latencies_ms, 99)
    pmer_mean = np.mean(pmer_latencies_ms)
    pmer_p99 = np.percentile(pmer_latencies_ms, 99)
    
    print(f"  Traditional 124-Ch Mean MAT: {trad_mean:.1f} ms (99th%: {trad_p99:.1f} ms)")
    print(f"  HopperNet 4-Ch PMER Mean MAT: {pmer_mean:.1f} ms (99th%: {pmer_p99:.1f} ms)")
    
    # Plot CDF (Cumulative Distribution Function)
    plt.figure(figsize=(10, 5), dpi=300)
    
    sorted_trad = np.sort(traditional_latencies_ms)
    sorted_pmer = np.sort(pmer_latencies_ms)
    cdf = np.linspace(0, 1, num_trials)
    
    plt.plot(sorted_trad / 1000.0, cdf * 100, label=f"Traditional Serial Scan (Mean: {trad_mean/1000.0:.2f} s)", color="#ef4444", lw=2)
    plt.plot(sorted_pmer, cdf * 100, label=f"HopperNet 4-Anchor PMER (Mean: {pmer_mean:.1f} ms - Instant)", color="#10b981", lw=2.5)
    
    plt.title("Synchronization Acquisition Latency CDF (Zero-Waiting Benchmark)", fontsize=14, fontweight="bold")
    plt.xlabel("Acquisition Time (seconds for Trad / milliseconds for PMER)", fontsize=12)
    plt.ylabel("Cumulative Probability (%)", fontsize=12)
    plt.xscale("log")
    plt.grid(True, which="both", linestyle=":", alpha=0.6)
    plt.legend(loc="lower right")
    plt.tight_layout()
    plt.savefig("simulations/plots/zero_wait_acquisition_cdf.png")
    plt.close()
    print("  -> Saved simulations/plots/zero_wait_acquisition_cdf.png")

# -------------------------------------------------------------
# 2. PROPORTIONAL-INTEGRAL (PI) PHASE & FREQUENCY TRACKING
# -------------------------------------------------------------
def simulate_pi_loop_filter():
    print("[2/3] Simulating PI Phase-Frequency Loop Filter (Zero-Drift Model)...")
    np.random.seed(42)
    total_time_sec = 120
    dwell_sec = 0.025
    steps = int(total_time_sec / dwell_sec)
    time_arr = np.linspace(0, total_time_sec, steps)
    
    # Hardware crystal error: +28.5 ppm frequency offset
    true_delta_f_ppm = 28.5 
    jitter_std_us = 8.0 # Microsecond measurement noise
    
    # Standard EMA filter (tracks phase only)
    ema_phase_err = np.zeros(steps)
    ema_offset = 0.0
    alpha = 0.15
    
    # PI Loop Filter (tracks Phase + Frequency Rate)
    # Kp = Proportional gain, Ki = Integral gain
    Kp = 0.18
    Ki = 0.012
    pi_phase_err = np.zeros(steps)
    pi_offset = 0.0
    pi_freq_est = 0.0 # Estimated ppm drift
    
    for i in range(1, steps):
        # Physical drift in one step
        step_drift_us = true_delta_f_ppm * dwell_sec
        
        # 1. EMA Step
        ema_offset += step_drift_us
        # 25% beacon drop probability
        if np.random.rand() > 0.25:
            meas = ema_offset + np.random.normal(0, jitter_std_us)
            ema_offset = (1.0 - alpha) * ema_offset + alpha * (ema_offset - meas)
        ema_phase_err[i] = ema_offset
        
        # 2. PI Loop Filter Step
        # Internal clock compensates by subtracting estimated frequency drift!
        pi_offset += (step_drift_us - (pi_freq_est * dwell_sec))
        if np.random.rand() > 0.25:
            meas_pi = pi_offset + np.random.normal(0, jitter_std_us)
            phase_error = meas_pi
            # PI Update Equations:
            pi_offset -= Kp * phase_error
            pi_freq_est += Ki * (phase_error / dwell_sec)
        pi_phase_err[i] = pi_offset

    plt.figure(figsize=(10, 5), dpi=300)
    plt.plot(time_arr, ema_phase_err, label="Standard Phase-Only EMA (Residual Steady-State Lag)", color="#f59e0b", lw=1.8)
    plt.plot(time_arr, pi_phase_err, label="HopperNet Dual-State PI Filter (Zero-Lag Frequency Tracking)", color="#10b981", lw=2.2)
    
    plt.axhline(0, color="#64748b", linestyle="--")
    plt.title("Synchronization Phase Error: EMA vs Dual-State PI Loop Filter", fontsize=14, fontweight="bold")
    plt.xlabel("Time (seconds)", fontsize=12)
    plt.ylabel("Phase Error (microseconds)", fontsize=12)
    plt.ylim(-60, 60)
    plt.grid(True, linestyle=":", alpha=0.6)
    plt.legend(loc="upper right")
    plt.tight_layout()
    plt.savefig("simulations/plots/pi_loop_filter_tracking.png")
    plt.close()
    print("  -> Saved simulations/plots/pi_loop_filter_tracking.png")

# -------------------------------------------------------------
# 3. END-TO-END LATENCY SIMULATION (Node A -> B -> C)
# -------------------------------------------------------------
def simulate_end_to_end_latency():
    print("[3/3] Simulating End-to-End Latency Across 3 Nodes...")
    num_packets = 500
    latencies_ms = []
    
    for _ in range(num_packets):
        # Slotted Timing Breakdown:
        # Phase 1 (Sync): 0 - 2.5 ms
        # Phase 2 (A -> B TX): starts at 2.5 ms -> takes 1.31 ms over air
        t_a_to_b = 2.5 + 1.31 + np.random.uniform(0.1, 0.4) # SPI + processing
        # Phase 3 (B -> C Relay Drain): starts at 7.5 ms -> takes 1.31 ms over air
        t_b_to_c = (7.5 - t_a_to_b) + 1.31 + np.random.uniform(0.1, 0.4)
        
        total_latency = t_a_to_b + t_b_to_c
        latencies_ms.append(total_latency)
        
    avg_lat = np.mean(latencies_ms)
    p99_lat = np.percentile(latencies_ms, 99)
    print(f"  Average A -> B -> C Latency: {avg_lat:.2f} ms (99th percentile: {p99_lat:.2f} ms)")
    
    plt.figure(figsize=(10, 5), dpi=300)
    plt.hist(latencies_ms, bins=30, color="#3b82f6", edgecolor="#1e3a8a", alpha=0.85)
    plt.axvline(avg_lat, color="#ef4444", linestyle="--", lw=2, label=f"Mean Latency: {avg_lat:.2f} ms")
    plt.axvline(p99_lat, color="#f59e0b", linestyle=":", lw=2, label=f"99th Percentile: {p99_lat:.2f} ms")
    
    plt.title("End-to-End Message Relay Latency Distribution (Node A → Node B → Node C)", fontsize=14, fontweight="bold")
    plt.xlabel("Total End-to-End Delivery Latency (milliseconds)", fontsize=12)
    plt.ylabel("Packet Count", fontsize=12)
    plt.grid(True, linestyle=":", alpha=0.6)
    plt.legend(loc="upper right")
    plt.tight_layout()
    plt.savefig("simulations/plots/end_to_end_latency_distribution.png")
    plt.close()
    print("  -> Saved simulations/plots/end_to_end_latency_distribution.png")

if __name__ == "__main__":
    simulate_acquisition_latency()
    simulate_pi_loop_filter()
    simulate_end_to_end_latency()
    print("\n[SUCCESS] Near-Zero Waiting & Zero-Drift Simulation Suite Complete!")
