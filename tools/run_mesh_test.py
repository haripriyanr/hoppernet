import serial
import time

def run_mesh_test():
    print("=" * 65)
    print(" HOPPERNET END-TO-END SINGLE CHANNEL TEST")
    print(" [NODE A (COM17)] ---> [NODE B (COM19)] ---> [NODE C (COM18)]")
    print("=" * 65)

    sa = serial.Serial('COM17', 115200, timeout=0.2)
    sb = serial.Serial('COM19', 115200, timeout=0.2)
    sc = serial.Serial('COM18', 115200, timeout=0.2)

    time.sleep(1.5)
    sa.reset_input_buffer()
    sb.reset_input_buffer()
    sc.reset_input_buffer()

    print("[*] Sending Test Packet 1 from Node A: 'FastMesh_Hello'...")
    sa.write(b"FastMesh_Hello\n")

    # Monitor for 3 seconds
    t_end = time.time() + 3.0
    while time.time() < t_end:
        if sa.in_waiting:
            line = sa.readline().decode('utf-8', errors='ignore').strip()
            if line: print(f"  [NODE A] {line}")
        if sb.in_waiting:
            line = sb.readline().decode('utf-8', errors='ignore').strip()
            if line: print(f"  \033[94m[NODE B (RELAY)] {line}\033[0m")
        if sc.in_waiting:
            line = sc.readline().decode('utf-8', errors='ignore').strip()
            if line: print(f"  \033[92m[NODE C (DESTINATION)] {line}\033[0m")
        time.sleep(0.01)

    print("\n[*] Sending 20-Packet High-Speed BURST from Node A...")
    sa.write(b"BURST\n")

    t_end = time.time() + 4.0
    while time.time() < t_end:
        if sa.in_waiting:
            line = sa.readline().decode('utf-8', errors='ignore').strip()
            if line: print(f"  [NODE A] {line}")
        if sb.in_waiting:
            line = sb.readline().decode('utf-8', errors='ignore').strip()
            if line: print(f"  \033[94m[NODE B (RELAY)] {line}\033[0m")
        if sc.in_waiting:
            line = sc.readline().decode('utf-8', errors='ignore').strip()
            if line: print(f"  \033[92m[NODE C (DESTINATION)] {line}\033[0m")
        time.sleep(0.01)

    sa.close()
    sb.close()
    sc.close()
    print("\n[+] End-to-end test finished.")

if __name__ == "__main__":
    run_mesh_test()
