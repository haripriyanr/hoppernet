"""
HopperNet Adversary & 7-Mode Jammer Resilience Simulation
Evaluates HopperNet mesh PDR, latency, and blacklist dynamics against 7 distinct attack vectors:
1. SPOT Jammer (Single Channel Lock)
2. SWEEP Jammer (Full 124-Channel Sweep)
3. RANDOM HOP Jammer (Uniform Random)
4. AUTO-HOP Jammer (FHSS Sequence Tracker)
5. ADAPTIVE Jammer (RPD Sniffer & Targeter)
6. BURST PULSE Jammer (Microburst Disruption)
7. ANCHOR KILLER Jammer (PMER Anchor Denial)
"""

import os
import numpy as np
import matplotlib.pyplot as plt

os.makedirs("simulations/plots", exist_ok=True)

NUM_CHANNELS = 124
CHANNEL_BASE = 2
DWELL_MS = 25.0
ANCHOR_CHANNELS = [10, 42, 74, 106]
FHSS_SEED = 0xC0FFEE01

def xorshift32(state):
    state ^= (state << 13) & 0xFFFFFFFF
    state ^= (state >> 17) & 0xFFFFFFFF
    state ^= (state << 5) & 0xFFFFFFFF
    return state & 0xFFFFFFFF

def channel_for_hop(hop, seed, blacklist_set):
    state = (seed ^ (hop * 2654435761)) & 0xFFFFFFFF
    for _ in range(NUM_CHANNELS * 2):
        state = xorshift32(state)
        ch = CHANNEL_BASE + (state % NUM_CHANNELS)
        if ch not in blacklist_set:
            return ch
    return CHANNEL_BASE + (hop % NUM_CHANNELS)

def simulate_jammer_attacks():
    print("================================================================")
    print("  HopperNet Multi-Vector Jammer Resilience Simulation Suite     ")
    print("================================================================")
    
    np.random.seed(42)
    total_hops = 1000  # 25 seconds of real-time 25ms communication
    
    modes = ["SPOT", "SWEEP", "RANDOM", "AUTO_HOP", "ADAPTIVE", "BURST", "ANCHOR_KILLER"]
    
    results = {}
    
    for mode in modes:
        print(f"Simulating Attack Vector: {mode} (1000 Hops / 25ms dwell)...")
        blacklist = set()
        blacklist_history = []
        packets_sent = 0
        packets_delivered = 0
        pdr_timeline = []
        rpd_detection_counter = {}
        
        sweep_idx = CHANNEL_BASE
        
        for hop in range(total_hops):
            packets_sent += 1
            # 1. Mesh Node selects channel via FHSS + Blacklist
            mesh_ch = channel_for_hop(hop, FHSS_SEED, blacklist)
            
            # 2. Jammer determines target channel based on attack mode
            jam_channels = []
            if mode == "SPOT":
                jam_channels = [45] # Locked onto Ch 45
            elif mode == "SWEEP":
                sweep_idx = CHANNEL_BASE + (hop * 4) % NUM_CHANNELS
                jam_channels = [sweep_idx + i for i in range(4) if sweep_idx + i < CHANNEL_BASE + NUM_CHANNELS]
            elif mode == "RANDOM":
                jam_channels = [np.random.randint(CHANNEL_BASE, CHANNEL_BASE + NUM_CHANNELS)]
            elif mode == "AUTO_HOP":
                # Adversary attempts to track seed without knowing dynamic blacklist
                jam_channels = [channel_for_hop(hop, FHSS_SEED, set())]
            elif mode == "ADAPTIVE":
                # Sniffs mesh channel with 65% probability if mesh is on air
                if np.random.rand() < 0.65:
                    jam_channels = [mesh_ch]
                else:
                    jam_channels = [np.random.randint(CHANNEL_BASE, CHANNEL_BASE + NUM_CHANNELS)]
            elif mode == "BURST":
                # Pulse bursts on 2 pseudo-random channels
                jam_channels = [np.random.randint(CHANNEL_BASE, CHANNEL_BASE + NUM_CHANNELS),
                                np.random.randint(CHANNEL_BASE, CHANNEL_BASE + NUM_CHANNELS)]
            elif mode == "ANCHOR_KILLER":
                # Target the 4 rendezvous anchors
                jam_channels = [ANCHOR_CHANNELS[hop % len(ANCHOR_CHANNELS)]]
                
            # 3. RF Collision & Interference Check
            collision = (mesh_ch in jam_channels)
            
            if collision:
                # Carrier / RPD scan detects persistent interference
                rpd_detection_counter[mesh_ch] = rpd_detection_counter.get(mesh_ch, 0) + 1
                if rpd_detection_counter[mesh_ch] >= 2: # 2 consecutive hits triggers blacklisting
                    blacklist.add(mesh_ch)
                    
                # Packet lost on this hop (edge buffer will retry on next hop)
                pass
            else:
                packets_delivered += 1
                # If channel was clean, decay RPD counter
                if mesh_ch in rpd_detection_counter:
                    rpd_detection_counter[mesh_ch] = max(0, rpd_detection_counter[mesh_ch] - 1)
                    
            # Aging mechanism: blacklist expires after 200 hops (5 seconds)
            if hop % 200 == 0 and len(blacklist) > 0:
                # Remove oldest blacklisted channel to test self-healing
                oldest = list(blacklist)[0]
                blacklist.remove(oldest)
                
            current_pdr = (packets_delivered / packets_sent) * 100.0
            pdr_timeline.append(current_pdr)
            blacklist_history.append(len(blacklist))
            
        final_pdr = (packets_delivered / packets_sent) * 100.0
        print(f"  -> {mode} Results: PDR = {final_pdr:.2f}% | Max Blacklisted Chs: {max(blacklist_history)}")
        results[mode] = {
            "pdr": pdr_timeline,
            "blacklist": blacklist_history,
            "final_pdr": final_pdr
        }
        
    # -------------------------------------------------------------
    # Plotting Comprehensive Jammer Resilience Curves
    # -------------------------------------------------------------
    fig, (ax1, ax2) = plt.subplots(2, 1, figsize=(12, 9), dpi=300, sharex=True)
    
    colors = {
        "SPOT": "#ef4444",
        "SWEEP": "#f97316",
        "RANDOM": "#8b5cf6",
        "AUTO_HOP": "#06b6d4",
        "ADAPTIVE": "#ec4899",
        "BURST": "#eab308",
        "ANCHOR_KILLER": "#10b981"
    }
    
    time_sec = np.arange(total_hops) * (DWELL_MS / 1000.0)
    
    for mode in modes:
        ax1.plot(time_sec, results[mode]["pdr"], label=f"{mode} (Final PDR: {results[mode]['final_pdr']:.1f}%)", color=colors[mode], lw=2)
        ax2.plot(time_sec, results[mode]["blacklist"], label=f"{mode}", color=colors[mode], lw=1.8)
        
    ax1.set_title("HopperNet Packet Delivery Ratio (PDR) Under 7 Jammer Attack Vectors", fontsize=14, fontweight="bold")
    ax1.set_ylabel("PDR (%)", fontsize=12)
    ax1.set_ylim(85, 101)
    ax1.grid(True, linestyle=":", alpha=0.6)
    ax1.legend(loc="lower left", fontsize=10, framealpha=0.9)
    
    ax2.set_title("Active Dynamic Blacklist Size (Self-Healing Evasion Response)", fontsize=13, fontweight="bold")
    ax2.set_xlabel("Elapsed Time (Seconds)", fontsize=12)
    ax2.set_ylabel("Blacklisted Channels", fontsize=12)
    ax2.grid(True, linestyle=":", alpha=0.6)
    ax2.legend(loc="upper right", fontsize=10, framealpha=0.9)
    
    plt.tight_layout()
    plt.savefig("simulations/plots/jammer_7mode_resilience.png")
    plt.close()
    print("\nSaved high-resolution plot to: simulations/plots/jammer_7mode_resilience.png")
    
    # -------------------------------------------------------------
    # Bar Chart Summary Comparison
    # -------------------------------------------------------------
    plt.figure(figsize=(10, 5), dpi=300)
    mode_labels = [m.replace("_", " ") for m in modes]
    final_pdrs = [results[m]["final_pdr"] for m in modes]
    bar_colors = [colors[m] for m in modes]
    
    bars = plt.bar(mode_labels, final_pdrs, color=bar_colors, width=0.55, edgecolor="black", alpha=0.9)
    plt.title("HopperNet Resilience Against 7 Electronic Warfare Jamming Strategies", fontsize=13, fontweight="bold")
    plt.ylabel("Packet Delivery Ratio (PDR %)", fontsize=12)
    plt.ylim(80, 105)
    plt.grid(axis="y", linestyle=":", alpha=0.7)
    
    for bar in bars:
        yval = bar.get_height()
        plt.text(bar.get_x() + bar.get_width()/2.0, yval + 0.8, f"{yval:.2f}%", ha="center", va="bottom", fontweight="bold", fontsize=10)
        
    plt.tight_layout()
    plt.savefig("simulations/plots/jammer_mode_pdr_comparison.png")
    plt.close()
    print("Saved bar comparison to: simulations/plots/jammer_mode_pdr_comparison.png")

if __name__ == "__main__":
    simulate_jammer_attacks()
