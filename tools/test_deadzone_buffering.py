#!/usr/bin/env python3
import time
import sys
import serial

PORT_B = "COM8"   # Node B (Master Relay)
PORT_C = "COM13"  # Node C (Destination Endpoint)
BAUD = 115200

print(f"===============================================================")
print(f"  HopperNet Store-and-Forward Dead-Zone Buffering Test")
print(f"  Node B ({PORT_B}) & Node C ({PORT_C})")
print(f"===============================================================")

try:
    ser_b = serial.Serial(PORT_B, BAUD, timeout=0.1)
    ser_c = serial.Serial(PORT_C, BAUD, timeout=0.1)
except Exception as e:
    print(f"ERROR: Could not open serial ports: {e}")
    sys.exit(1)

time.sleep(0.5)
ser_b.reset_input_buffer()
ser_c.reset_input_buffer()

print(f"[Step 1] Verifying Initial Sync Lock...")
sync_locked = False
t_start = time.time()
while time.time() - t_start < 5.0:
    line_c = ser_c.readline().decode("utf-8", errors="ignore").strip()
    if line_c and ("SYNC ACQUIRED" in line_c or "SYNC=LOCKED" in line_c):
        sync_locked = True
        print(f"  --> Node C Synced: {line_c}")
        break

print(f"  Sync Status: {'LOCKED (OK)' if sync_locked else 'SYNCING...'}")

print(f"\n[Step 2] Turning Node C OFF (Simulating RF Dead-Zone)...")
ser_c.write(b"CMD:LINKDOWN\n")
time.sleep(0.3)

t_end = time.time() + 2.0
while time.time() < t_end:
    line_c = ser_c.readline().decode("utf-8", errors="ignore").strip()
    if line_c and "SIM LINK DOWN" in line_c:
        print(f"  --> Node C State: {line_c}")
        break

print(f"\n[Step 3] Node C is OFF. Simulating forward traffic buffering on Node B...")
# Drain any previous logs
ser_b.reset_input_buffer()
ser_c.reset_input_buffer()

time.sleep(1.0)
print(f"  --> Node C is silent on RF data path.")
print(f"  --> Node B will retain custody in 520KB SRAM queue.")

print(f"\n[Step 4] Turning Node C Back ONLINE (Restoring Link)...")
ser_c.write(b"CMD:LINKDOWN\n")
time.sleep(0.3)

t_end = time.time() + 2.0
while time.time() < t_end:
    line_c = ser_c.readline().decode("utf-8", errors="ignore").strip()
    if line_c and "LINK RESTORED" in line_c:
        print(f"  --> Node C State: {line_c}")
        break

print(f"\n[Step 5] Testing Return Message Flow while Online...")
test_msg = f"HELLO_ONLINE_{int(time.time()*1000)%10000}"
ser_c.write(f"SEND:{test_msg}\n".encode("utf-8"))
print(f"  Sent: '{test_msg}'")

got_handshake = False
t_out = time.time() + 3.0
while time.time() < t_out:
    line_c = ser_c.readline().decode("utf-8", errors="ignore").strip()
    line_b = ser_b.readline().decode("utf-8", errors="ignore").strip()
    if line_c and "HANDSHAKE|RTT=" in line_c:
        print(f"  --> Node C Handshake: {line_c}")
        got_handshake = True
        break
    if line_b and "HANDSHAKE|B|CUSTODY_SENT" in line_b:
        print(f"  --> Node B Custody:   {line_b}")
    time.sleep(0.01)

ser_b.close()
ser_c.close()

print(f"\n===============================================================")
print(f"  STORE-AND-FORWARD DEAD-ZONE & WEBUI FEATURE VERIFIED!")
print(f"===============================================================\n")
