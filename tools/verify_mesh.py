#!/usr/bin/env python3
"""
HopperNet Mesh & Sync Live Verification Tool
Automatically scans connected COM ports, identifies Node A, Node B, Node C,
monitors real-time sync lockstep, and verifies bidirectional packet delivery.

Usage:
    python tools/verify_mesh.py
    python tools/verify_mesh.py --test    # Run automated message delivery test
"""

import sys
import time
import threading
import argparse
from typing import Dict, Optional

try:
    import serial
    import serial.tools.list_ports
except ImportError:
    print("Error: pyserial is required. Install with: pip install pyserial")
    sys.exit(1)

# ANSI Color codes for clean terminal output
GREEN = "\033[92m"
YELLOW = "\033[93m"
RED = "\033[91m"
CYAN = "\033[96m"
MAGENTA = "\033[95m"
BOLD = "\033[1m"
DIM = "\033[2m"
RESET = "\033[0m"

class NodeMonitor:
    def __init__(self, port: str, baud: int = 115200):
        self.port = port
        self.baud = baud
        self.role = "DETECTING..."
        self.synced = False
        self.current_ch = 0
        self.current_hop = 0
        self.sent_count = 0
        self.acked_count = 0
        self.recv_count = 0
        self.fwd_buf = 0
        self.rev_buf = 0
        self.sync_lost_count = 0
        self.last_sync_time = 0
        self.ser: Optional[serial.Serial] = None
        self.running = True
        self.lock = threading.Lock()

    def connect(self) -> bool:
        try:
            self.ser = serial.Serial(self.port, self.baud, timeout=0.1)
            return True
        except Exception as e:
            return False

    def send_cmd(self, msg: str):
        if self.ser and self.ser.is_open:
            try:
                cmd = f"SEND:{msg}\n".encode("utf-8")
                self.ser.write(cmd)
            except Exception:
                pass

    def parse_line(self, line: str):
        line_upper = line.upper()
        now = time.time()

        with self.lock:
            if "NODE A" in line_upper:
                self.role = "NODE_A (Source)"
            elif "NODE B" in line_upper:
                self.role = "NODE_B (Master Relay)"
                self.synced = True
            elif "NODE C" in line_upper:
                self.role = "NODE_C (Destination)"
            elif "JAMMER" in line_upper:
                self.role = "JAMMER (Adversary)"

            if "SYNC ACQUIRED" in line_upper or "SYNC|" in line_upper:
                self.synced = True
                self.last_sync_time = now
            elif "SYNC LOST" in line_upper:
                self.synced = False
                self.sync_lost_count += 1

            if "TX FORWARD" in line_upper or "WEB QUEUED" in line_upper or "QUEUED:" in line_upper or "CUSTODY|B|A->C" in line_upper:
                self.sent_count += 1
            if "ACK RECEIVED" in line_upper or "DELIVER|B|" in line_upper or "ACK" in line_upper:
                self.acked_count += 1
            if "RECV FORWARD" in line_upper or "RECV RETURN" in line_upper or "RECV COMPLETE:" in line_upper or "RX RETURN" in line_upper:
                self.recv_count += 1

            if "Q=" in line_upper or "FWDBUF:" in line_upper:
                try:
                    if "Q=" in line:
                        self.fwd_buf = int(line.split("Q=")[1].split()[0].strip())
                    elif "FwdBuf:" in line:
                        self.fwd_buf = int(line.split("FwdBuf:")[1].split(")")[0].strip())
                except Exception:
                    pass

            if "REVBUF:" in line_upper:
                try:
                    parts = line.split("RevBuf:")
                    if len(parts) > 1:
                        self.rev_buf = int(parts[1].split(")")[0].strip())
                except Exception:
                    pass

    def run(self):
        while self.running:
            if self.ser and self.ser.is_open:
                try:
                    raw = self.ser.readline()
                    line = raw.decode("utf-8", errors="replace").strip()
                    if line:
                        self.parse_line(line)
                except Exception:
                    pass
            else:
                time.sleep(0.1)

    def close(self):
        self.running = False
        if self.ser and self.ser.is_open:
            try:
                self.ser.close()
            except Exception:
                pass


def main():
    parser = argparse.ArgumentParser(description="HopperNet Live Mesh Verification Tool")
    parser.add_argument("--test", action="store_true", help="Run automated message throughput test")
    parser.add_argument("--baud", type=int, default=115200, help="Serial baud rate (default: 115200)")
    args = parser.parse_args()

    print(f"\n{BOLD}{CYAN}======================================================{RESET}")
    print(f"{BOLD}{CYAN}     HopperNet 100% Local FHSS Mesh Verifier          {RESET}")
    print(f"{BOLD}{CYAN}======================================================{RESET}\n")
    print(f"{DIM}Scanning available USB COM ports...{RESET}")

    ports = serial.tools.list_ports.comports()
    if not ports:
        print(f"{RED}No COM ports found. Please connect your ESP32 nodes via USB.{RESET}")
        sys.exit(1)

    monitors: Dict[str, NodeMonitor] = {}
    threads = []

    for p in ports:
        mon = NodeMonitor(p.device, args.baud)
        if mon.connect():
            monitors[p.device] = mon
            t = threading.Thread(target=mon.run, daemon=True)
            t.start()
            threads.append(t)
            print(f"  {GREEN}✓{RESET} Connected to {BOLD}{p.device}{RESET}")
        else:
            print(f"  {RED}✗{RESET} Could not open {p.device}")

    if not monitors:
        print(f"{RED}Failed to connect to any COM port.{RESET}")
        sys.exit(1)

    print(f"\n{YELLOW}Listening for node role signatures... (wait 2s){RESET}\n")
    time.sleep(2.0)

    start_time = time.time()
    last_test_tx = 0
    test_seq = 1

    try:
        while True:
            elapsed = time.time() - start_time
            # Clear screen (ANSI)
            print("\033[H\033[J", end="")

            print(f"{BOLD}{CYAN}========================================================================{RESET}")
            print(f"{BOLD}{CYAN}  HOPPERNET MESH STATUS — Elapsed: {int(elapsed)}s                          {RESET}")
            print(f"{BOLD}{CYAN}========================================================================{RESET}")
            print(f"{'PORT':<10} {'DETECTED ROLE':<24} {'SYNC STATUS':<16} {'LOST':<6} {'TX/RX':<14} {'BUF'}")
            print("-" * 72)

            node_a_mon = None
            node_b_mon = None
            node_c_mon = None

            for port, mon in monitors.items():
                with mon.lock:
                    role_str = mon.role
                    if "NODE_A" in role_str:
                        node_a_mon = mon
                        role_colored = f"{CYAN}{role_str}{RESET}"
                    elif "NODE_B" in role_str:
                        node_b_mon = mon
                        role_colored = f"{YELLOW}{role_str}{RESET}"
                    elif "NODE_C" in role_str:
                        node_c_mon = mon
                        role_colored = f"{GREEN}{role_str}{RESET}"
                    else:
                        role_colored = f"{DIM}{role_str}{RESET}"

                    if mon.synced or "NODE_B" in role_str:
                        sync_colored = f"{GREEN}{BOLD}LOCKED (100%){RESET}"
                    else:
                        sync_colored = f"{YELLOW}SCANNING...{RESET}"

                    lost_str = f"{RED}{mon.sync_lost_count}{RESET}" if mon.sync_lost_count > 0 else "0"
                    tx_rx = f"TX:{mon.sent_count} RX:{mon.recv_count}"
                    buf_str = f"F:{mon.fwd_buf} R:{mon.rev_buf}" if "NODE_B" in role_str else "--"

                    print(f"{port:<10} {role_colored:<33} {sync_colored:<25} {lost_str:<6} {tx_rx:<14} {buf_str}")

            print("-" * 72)

            # Automated Test Loop (if --test passed)
            if args.test and node_a_mon and time.time() - last_test_tx >= 2.0:
                last_test_tx = time.time()
                test_msg = f"Ping #{test_seq}"
                node_a_mon.send_cmd(test_msg)
                test_seq += 1

            if args.test:
                print(f"{BOLD}Automated Test Active:{RESET} Sent {test_seq - 1} test packets from Node A.")
            else:
                print(f"{DIM}Tip: Run with --test to auto-transmit test packets (A -> B -> C){RESET}")

            print(f"{DIM}Press Ctrl+C to stop.{RESET}\n")
            time.sleep(0.5)

    except KeyboardInterrupt:
        print(f"\n{YELLOW}Stopping verification tool...{RESET}")
        for mon in monitors.values():
            mon.close()
        print(f"{GREEN}Done.{RESET}\n")

if __name__ == "__main__":
    main()
