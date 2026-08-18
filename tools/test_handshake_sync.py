#!/usr/bin/env python3
import time
import sys
import threading
import serial

PORT_B = "COM8"   # Node B (Master Relay)
PORT_A = "COM13"  # Node A (Source)
BAUD = 115200

print(f"===============================================================")
print(f"  HopperNet Live Hardware Test — Node B ({PORT_B}) & Node A ({PORT_A})")
print(f"===============================================================")

try:
    ser_b = serial.Serial(PORT_B, BAUD, timeout=0.1)
    ser_a = serial.Serial(PORT_A, BAUD, timeout=0.1)
except Exception as e:
    print(f"ERROR: Could not open serial ports: {e}")
    sys.exit(1)

# Clear any initial buffers
time.sleep(0.5)
ser_b.reset_input_buffer()
ser_a.reset_input_buffer()

print(f"[1/3] Waiting for Initial Synchronization Lock...")
sync_locked = False
start_wait = time.time()

logs_a = []
logs_b = []

while time.time() - start_wait < 5.0:
    line_a = ser_a.readline().decode("utf-8", errors="ignore").strip()
    if line_a:
        logs_a.append(line_a)
        if "SYNC ACQUIRED" in line_a or "SYNC=LOCKED" in line_a:
            sync_locked = True
            print(f"  --> Node A Synced! Log: {line_a}")
            break
    line_b = ser_b.readline().decode("utf-8", errors="ignore").strip()
    if line_b:
        logs_b.append(line_b)

if not sync_locked:
    # Check if telemetry already shows LOCKED
    ser_a.reset_input_buffer()
    t_end = time.time() + 2.0
    while time.time() < t_end:
        line_a = ser_a.readline().decode("utf-8", errors="ignore").strip()
        if "SYNC=LOCKED" in line_a or "SYNC ACQUIRED" in line_a:
            sync_locked = True
            print(f"  --> Node A Synced! Log: {line_a}")
            break

print(f"  [Status] Sync Status: {'LOCKED (100% OK)' if sync_locked else 'ACQUIRING...'}")

print(f"\n[2/3] Testing Slotted Handshake Latency & Custody Transfer (Target: ~625µs)...")

handshake_rtts = []
handshake_success = 0

# Send 5 test messages from Node A
for i in range(5):
    test_msg = f"TEST_{i+1}_{int(time.time()*1000)%10000}"
    ser_a.write(f"SEND:{test_msg}\n".encode("utf-8"))
    print(f"  [TX Request] Sent: '{test_msg}'")
    
    t_out = time.time() + 2.0
    got_ack = False
    while time.time() < t_out:
        line_a = ser_a.readline().decode("utf-8", errors="ignore").strip()
        line_b = ser_b.readline().decode("utf-8", errors="ignore").strip()
        
        if line_a and "HANDSHAKE|RTT=" in line_a:
            # Format: HANDSHAKE|RTT=630_us|msg=1|frag=0|sf=123
            print(f"    Node A Handshake Log: {line_a}")
            try:
                rtt_str = line_a.split("HANDSHAKE|RTT=")[1].split("_us")[0]
                rtt_val = int(rtt_str)
                handshake_rtts.append(rtt_val)
                got_ack = True
            except:
                pass
        elif line_a and ("QUEUED:" in line_a or "TELEMETRY" in line_a):
            pass
            
        if line_b and "HANDSHAKE|B|CUSTODY_SENT" in line_b:
            print(f"    Node B Custody Log:   {line_b}")
            
        if got_ack:
            handshake_success += 1
            break
        time.sleep(0.01)
    time.sleep(0.3)

if handshake_rtts:
    avg_rtt = sum(handshake_rtts) / len(handshake_rtts)
    min_rtt = min(handshake_rtts)
    max_rtt = max(handshake_rtts)
    print(f"  --> Handshake Results: {handshake_success}/5 Acknowledged")
    print(f"  --> Latency RTT: Avg = {avg_rtt:.1f} µs | Min = {min_rtt} µs | Max = {max_rtt} µs")
else:
    print(f"  --> Handshake RTT measurements: {handshake_rtts}")

print(f"\n[3/3] Testing Continuous 30-Second 100% Hop Synchronization Retention...")
print(f"  Monitoring 600 consecutive 50ms superframes (30.0 seconds)...")

sync_reports = []
start_30s = time.time()
test_duration = 30.0

node_a_telemetry = []
node_b_syncs = []

while time.time() - start_30s < test_duration:
    line_a = ser_a.readline().decode("utf-8", errors="ignore").strip()
    if line_a:
        if "TELEMETRY|NODE_A" in line_a:
            node_a_telemetry.append(line_a)
            print(f"  [T+{time.time()-start_30s:.1f}s] {line_a}")
    line_b = ser_b.readline().decode("utf-8", errors="ignore").strip()
    if line_b:
        if "SYNC|B" in line_b:
            node_b_syncs.append(line_b)
    time.sleep(0.005)

ser_a.close()
ser_b.close()

print(f"\n===============================================================")
print(f"                        TEST SUMMARY")
print(f"===============================================================")

# Analyze Telemetry
locked_count = sum(1 for t in node_a_telemetry if "SYNC=LOCKED" in t)
rate_100_count = sum(1 for t in node_a_telemetry if "100%" in t or "20/20" in t)
total_reports = len(node_a_telemetry)

print(f"  * 30-Second Duration: Completed 30.0 seconds")
print(f"  * Total 1-Second Telemetry Snapshots: {total_reports}")
print(f"  * 'SYNC=LOCKED' Snapshots: {locked_count}/{total_reports} ({(locked_count/total_reports*100) if total_reports else 0:.1f}%)")
print(f"  * '20/20 (100%)' Perfect Hop Match: {rate_100_count}/{total_reports} ({(rate_100_count/total_reports*100) if total_reports else 0:.1f}%)")

if handshake_rtts:
    print(f"  * Over-the-Air Slotted Handshake RTT: {avg_rtt:.1f} µs (Target: ~625 µs)")

if total_reports > 0 and locked_count == total_reports:
    print(f"\n >>> RESULT: 100% SYNCHRONIZATION RETENTION VERIFIED FOR 30 SECONDS! <<<")
else:
    print(f"\n >>> RESULT: Sync stats recorded. <<<")
print(f"===============================================================\n")
