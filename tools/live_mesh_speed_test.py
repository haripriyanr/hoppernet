import serial
import threading
import time
import sys

# Ports:
PORT_A = "COM17"
PORT_B = "COM19"
PORT_C = "COM18"
BAUD = 115200

def reader_thread(name, port, color_code):
    try:
        ser = serial.Serial(port, BAUD, timeout=1)
        print(f"[+] Connected to {name} on {port}")
        while True:
            line = ser.readline().decode('utf-8', errors='replace').strip()
            if line:
                print(f"\033[{color_code}m[{name} | {port}]\033[0m {line}")
    except Exception as e:
        print(f"[!] Error on {name} ({port}): {e}")

def main():
    print("=" * 65)
    print(" HOPPERNET SINGLE CHANNEL (CH 76) LIVE SPEED MONITOR")
    print(" Flow: [NODE A] ---> [NODE B (RELAY)] ---> [NODE C (DEST)]")
    print("=" * 65)

    t_a = threading.Thread(target=reader_thread, args=("NODE A", PORT_A, "92"), daemon=True) # Green
    t_b = threading.Thread(target=reader_thread, args=("NODE B", PORT_B, "94"), daemon=True) # Blue
    t_c = threading.Thread(target=reader_thread, args=("NODE C", PORT_C, "93"), daemon=True) # Yellow

    t_a.start()
    t_b.start()
    t_c.start()

    time.sleep(2)
    print("\n[*] All 3 serial monitors active. Listening to transmissions...")
    print("[*] Type any message and press ENTER to send from Node A:")
    print("[*] Type 'BURST' to run 20-packet high speed throughput benchmark:")
    print("-----------------------------------------------------------------")

    try:
        ser_a = serial.Serial(PORT_A, BAUD, timeout=1)
        while True:
            cmd = input()
            if cmd.strip().lower() in ['exit', 'quit']:
                break
            if cmd:
                ser_a.write((cmd + "\n").encode('utf-8'))
    except KeyboardInterrupt:
        print("\n[*] Exiting...")

if __name__ == "__main__":
    main()
