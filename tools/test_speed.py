import serial
import time

try:
    sa = serial.Serial('COM17', 115200, timeout=0.5)
    sb = serial.Serial('COM19', 115200, timeout=0.5)
    sc = serial.Serial('COM18', 115200, timeout=0.5)

    time.sleep(1.0)
    # Drain old buffers
    sa.reset_input_buffer()
    sb.reset_input_buffer()
    sc.reset_input_buffer()

    print("[*] Triggering test message from Node A...")
    sa.write(b"BURST\n")

    time.sleep(2.0)

    print("\n--- [NODE A OUTPUT] ---")
    while sa.in_waiting > 0:
        print("  A:", sa.readline().decode('utf-8', errors='ignore').strip())

    print("\n--- [NODE B (RELAY) OUTPUT] ---")
    while sb.in_waiting > 0:
        print("  B:", sb.readline().decode('utf-8', errors='ignore').strip())

    print("\n--- [NODE C (DESTINATION) OUTPUT] ---")
    while sc.in_waiting > 0:
        print("  C:", sc.readline().decode('utf-8', errors='ignore').strip())

    sa.close()
    sb.close()
    sc.close()

except Exception as e:
    print(f"[!] Error: {e}")
