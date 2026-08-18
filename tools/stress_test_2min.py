#!/usr/bin/env python3
"""
HopperNet 2-Minute 625µs Micro-Slot Mesh Stress Test
Tracks 1,600 hops/sec synchronization, ChaCha20-Poly1305 end-to-end delivery, and RTT.
"""

import serial
import time
import sys
import threading

PORT_A = "COM13"
PORT_B = "COM8"
PORT_C = "COM14"
BAUD = 115200
DURATION_SEC = 120

print("=" * 65)
print("     HOPPERNET 2-MINUTE 625µs MICRO-SLOT STRESS TEST")
print("     Master Clock: 1,600 hops/sec (625µs / slot)")
print("     Security: ChaCha20-Poly1305 RFC 8439 (256-bit AEAD)")
print("     Nodes: A (COM13) -> B (COM8) -> C (COM14)")
print("=" * 65)

try:
    ser_a = serial.Serial(PORT_A, BAUD, timeout=0.1)
    ser_b = serial.Serial(PORT_B, BAUD, timeout=0.1)
    ser_c = serial.Serial(PORT_C, BAUD, timeout=0.1)
except Exception as e:
    print(f"ERROR: Could not open serial ports: {e}")
    sys.exit(1)

time.sleep(0.5)
ser_a.reset_input_buffer()
ser_b.reset_input_buffer()
ser_c.reset_input_buffer()

# Metrics
stats = {
    "sent_a": 0,
    "custody_b": 0,
    "delivered_c": 0,
    "auth_fails_c": 0,
    "last_sync_pct_a": 100.0,
    "last_sync_pct_c": 100.0,
    "last_sf_b": 0,
    "last_sf_a": 0,
    "last_sf_c": 0,
    "rtt_samples": [],
    "recv_messages": []
}

running = True

def reader_b():
    while running:
        try:
            line = ser_b.readline().decode(errors="ignore").strip()
            if not line:
                continue
            if "CUSTODY_SENT" in line:
                stats["custody_b"] += 1
            elif "SF=" in line:
                for part in line.split("|"):
                    if part.startswith("SF="):
                        try:
                            stats["last_sf_b"] = int(part.split("=")[1])
                        except Exception:
                            pass
        except Exception:
            pass

def reader_c():
    while running:
        try:
            line = ser_c.readline().decode(errors="ignore").strip()
            if not line:
                continue
            if "RECV COMPLETE" in line or "RECV_OK" in line or "DELIVERED" in line:
                stats["delivered_c"] += 1
                stats["recv_messages"].append(line)
            if "AUTH_FAIL" in line:
                stats["auth_fails_c"] += 1
            if "TELEMETRY|NODE_C" in line:
                for part in line.split("|"):
                    if part.startswith("PCT="):
                        try:
                            stats["last_sync_pct_c"] = float(part.split("=")[1])
                        except Exception:
                            pass
                    elif part.startswith("SF="):
                        try:
                            stats["last_sf_c"] = int(part.split("=")[1])
                        except Exception:
                            pass
        except Exception:
            pass

def reader_a():
    while running:
        try:
            line = ser_a.readline().decode(errors="ignore").strip()
            if not line:
                continue
            if "HANDSHAKE|RTT=" in line:
                try:
                    rtt = int(line.split("RTT=")[1].split("_us")[0])
                    stats["rtt_samples"].append(rtt)
                except Exception:
                    pass
            if "TELEMETRY|NODE_A" in line:
                for part in line.split("|"):
                    if part.startswith("PCT="):
                        try:
                            stats["last_sync_pct_a"] = float(part.split("=")[1])
                        except Exception:
                            pass
                    elif part.startswith("SF="):
                        try:
                            stats["last_sf_a"] = int(part.split("=")[1])
                        except Exception:
                            pass
        except Exception:
            pass

t_b = threading.Thread(target=reader_b, daemon=True)
t_c = threading.Thread(target=reader_c, daemon=True)
t_a = threading.Thread(target=reader_a, daemon=True)
t_b.start()
t_c.start()
t_a.start()

start_time = time.time()
last_report = start_time
last_send = 0
msg_counter = 1

print(f"\n[RUNNING] 2-minute test in progress for {DURATION_SEC} seconds...\n")

try:
    while time.time() - start_time < DURATION_SEC:
        now = time.time()
        elapsed = now - start_time
        
        # Send a forward message from Node A every 500 ms (2 msgs/sec)
        if now - last_send >= 0.5:
            last_send = now
            msg_text = f"SEND:HOP_TEST_{msg_counter}\n"
            ser_a.write(msg_text.encode())
            ser_a.flush()
            stats["sent_a"] += 1
            msg_counter += 1
            
        # Periodic report every 10 seconds
        if now - last_report >= 10.0:
            last_report = now
            avg_rtt = (sum(stats["rtt_samples"][-20:]) / len(stats["rtt_samples"][-20:])) if stats["rtt_samples"] else 0
            print(f"[{elapsed:5.1f}s / {DURATION_SEC}s] "
                  f"Sent (A): {stats['sent_a']} | "
                  f"Custody (B): {stats['custody_b']} | "
                  f"Delivered (C): {stats['delivered_c']} | "
                  f"Sync A: {stats['last_sync_pct_a']:.1f}% | "
                  f"Sync C: {stats['last_sync_pct_c']:.1f}% | "
                  f"Avg RTT: {avg_rtt:.0f} µs | "
                  f"Master SF: {stats['last_sf_b']}")
            
        time.sleep(0.01)

finally:
    running = False
    time.sleep(0.5)
    ser_a.close()
    ser_b.close()
    ser_c.close()

# Final Summary Report
elapsed_total = time.time() - start_time
print("\n" + "=" * 65)
print("               2-MINUTE STRESS TEST RESULTS")
print("=" * 65)
print(f"  Test Duration:               {elapsed_total:.1f} seconds")
print(f"  Total Master Hops Elapsed:   ~{int(elapsed_total * 1600):,} hops")
print(f"  Node A Sync Retention Rate:  {stats['last_sync_pct_a']:.1f}%")
print(f"  Node C Sync Retention Rate:  {stats['last_sync_pct_c']:.1f}%")
print(f"  Messages Dispatched (A):     {stats['sent_a']}")
print(f"  Custody ACKs from Relay (B): {stats['custody_b']}")
print(f"  Messages Delivered to C:     {stats['delivered_c']}")
print(f"  ChaCha20 Auth Failures:      {stats['auth_fails_c']} (0 expected)")
if stats["rtt_samples"]:
    print(f"  Min Inline RTT:              {min(stats['rtt_samples'])} µs")
    print(f"  Max Inline RTT:              {max(stats['rtt_samples'])} µs")
    print(f"  Avg Inline RTT:              {sum(stats['rtt_samples']) / len(stats['rtt_samples']):.1f} µs")
print("=" * 65)
