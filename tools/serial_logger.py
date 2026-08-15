#!/usr/bin/env python3
"""Capture serial output from a node into run/<name>.log.

Usage:
    python serial_logger.py --port COM8 --name node_a
    python serial_logger.py --port COM9 --name node_c
"""
import argparse
import os
import time
import sys

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", required=True, help="COM port, e.g. COM8")
    ap.add_argument("--name", required=True, help="log name, e.g. node_a")
    ap.add_argument("--baud", type=int, default=115200)
    args = ap.parse_args()

    try:
        import serial
    except ImportError:
        print("Need pyserial: pip install pyserial")
        sys.exit(1)

    run_dir = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "run")
    os.makedirs(run_dir, exist_ok=True)
    log_path = os.path.join(run_dir, f"{args.name}.log")

    ser = serial.Serial(args.port, args.baud, timeout=0.2)
    print(f"Logging {args.name} ({args.port}) -> {log_path}  [Ctrl+C to stop]")
    with open(log_path, "a", encoding="utf-8") as f:
        while True:
            try:
                line = ser.readline().decode("utf-8", errors="replace").rstrip()
                if line:
                    stamp = time.strftime("%H:%M:%S")
                    out = f"[{stamp}] {line}\n"
                    f.write(out)
                    f.flush()
                    print(out.rstrip())
            except KeyboardInterrupt:
                print("\nStopped.")
                break
            except serial.SerialException as e:
                print(f"Serial error: {e}")
                break

if __name__ == "__main__":
    main()
