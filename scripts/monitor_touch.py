import sys
import time
import serial

port_name = sys.argv[1] if len(sys.argv) > 1 else 'COM11'
baud = 115200
duration = int(sys.argv[2]) if len(sys.argv) > 2 else 5

print(f"Connecting to {port_name} at {baud} baud for {duration}s...")
try:
    ser = serial.Serial(port_name, baud, timeout=0.5)
    # Trigger DTR reset to capture boot output
    ser.setDTR(False)
    time.sleep(0.1)
    ser.setDTR(True)
    time.sleep(0.5)

    start_time = time.time()
    while time.time() - start_time < duration:
        if ser.in_waiting > 0:
            line = ser.readline().decode('utf-8', errors='ignore').strip()
            if line:
                print(f"[SERIAL] {line}")
        time.sleep(0.01)
    ser.close()
    print("Monitor closed.")
except Exception as e:
    print(f"Error: {e}")
