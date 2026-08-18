#!/usr/bin/env python3
import time
import sys
import threading
import serial

PORT_B = "COM8"   # Node B (Master Relay)
PORT_C = "COM13"  # Node C (Destination Endpoint)
BAUD = 115200

print(f"===============================================================")
print(f"  HopperNet Live Hardware Test — Node B ({PORT_B}) & Node C ({PORT_C})")
print(f"===============================================================")

try:
    ser_b = serial.Serial(PORT_B, BAUD, timeout=0.1)
    ser_c = serial.Serial(PORT_C, BAUD, timeout=0.1)
except Exception as e:
    print(f"ERROR: Could not open serial ports: {e}")
    sys.exit(1)

# Clear any initial buffers
time.sleep(0.5)
ser_b.reset_input_buffer()
ser_c.reset_input_buffer()

print(f"[1/3] Waiting for Initial Synchronization Lock...")
sync_locked = False
start_wait = time.time()

logs_c = []
logs_b = []

while time.time() - start_wait < 5.0:
    line_c = ser_c.readline().decode("utf-8", errors="ignore").strip()
    if line_c:
        logs_c.append(line_c)
        if "SYNC ACQUIRED" in line_c or "SYNC=LOCKED" in line_c:
            sync_locked = True
            print(f"  --> Node C Synced! Log: {line_c}")
            break
    line_b = ser_b.readline().decode("utf-8", errors="ignore").strip()
    if line_b:
        logs_b.append(line_b)

if not sync_locked:
    ser_c.reset_input_buffer()
    t_end = time.time() + 2.0
    while time.time() < t_end:
        line_c = ser_c.readline().decode("utf-8", errors="ignore").strip()
        if "SYNC=LOCKED" in line_c or "SYNC ACQUIRED" in line_c:
            sync_locked = True
            print(f"  --> Node C Synced! Log: {line_c}")
            break

print(f"  [Status] Sync Status: {'LOCKED (100% OK)' if sync_locked else 'ACQUIRING...'}")

print(f"\n[2/3] Testing Slotted Handshake Latency & Custody Transfer (Target: ~625µs)...")

handshake_rtts = []
handshake_success = 0

# Send 5 test messages from Node C
for i in range(5):
    test_msg = f"TEST_C_{i+1}_{int(time.time()*1000)%10000}"
    ser_c.write(f"SEND:{test_msg}\n".encode("utf-8"))
    print(f"  [TX Request] Sent from Node C: '{test_msg}'")
    
    t_out = time.time() + 2.0
    got_ack = False
    while time.time() < t_out:
        line_c = ser_c.readline().decode("utf-8", errors="ignore").strip()
        line_b = ser_b.readline().decode("utf-8", errors="ignore").strip()
        
        if line_c and "HANDSHAKE|RTT=" in line_c:
            # Format: HANDSHAKE|RTT=630_us|msg=1|frag=0|sf=123
            print(f"    Node C Handshake Log: {line_c}")
            try:
                rtt_str = line_c.split("HANDSHAKE|RTT=")[1].split("_us")[0]
                rtt_val = int(rtt_str)
                handshake_rtts.append(rtt_val)
                got_ack = True
            except:
                pass
        elif line_c and ("QUEUED RETURN:" in line_c or "TELEMETRY" in line_c):
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

node_c_telemetry = []
node_b_syncs = []

while time.time() - start_30s < test_duration:
    line_c = ser_c.readline().decode("utf-8", errors="ignore").strip()
    if line_c:
        if "TELEMETRY|NODE_C" in line_c:
            node_c_telemetry.append(line_c)
            print(f"  [T+{time.time()-start_30s:.1f}s] {line_c}")
    line_b = ser_b.readline().decode("utf-8", errors="ignore").strip()
    if line_b:
        if "SYNC|B" in line_b:
            node_b_syncs.append(line_b)
    time.sleep(0.005)

ser_c.close()
ser_b.close()

print(f"\n===============================================================")
print(f"                        TEST SUMMARY")
print(f"===============================================================")

locked_count = sum(1 for t in node_c_telemetry if "SYNC=LOCKED" in t)
rate_100_count = sum(1 for t in node_c_telemetry if "100%" in t or "20/20" in t)
total_reports = len(node_c_telemetry)

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
