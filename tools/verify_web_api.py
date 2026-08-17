#!/usr/bin/env python3
"""
HopperNet Wi-Fi Web API Live Verification Tool
Connects to the local ESP32 Web Portal (http://192.168.4.1) over Wi-Fi SoftAP
and verifies real-time 40-hop spectrum synchronization, queue depth, and messaging.

Usage:
    python tools/verify_web_api.py
    python tools/verify_web_api.py --send "Test Alert 123"
    python tools/verify_web_api.py --host 192.168.4.1
"""

import sys
import time
import argparse
import urllib.request
import urllib.parse
import json

GREEN = "\033[92m"
YELLOW = "\033[93m"
RED = "\033[91m"
CYAN = "\033[96m"
BOLD = "\033[1m"
DIM = "\033[2m"
RESET = "\033[0m"

def fetch_status(host: str) -> dict:
    url = f"http://{host}/api/status"
    req = urllib.request.Request(url, headers={'User-Agent': 'HopperNet-Verifier/1.0'})
    with urllib.request.urlopen(req, timeout=1.5) as resp:
        if resp.status == 200:
            return json.loads(resp.read().decode('utf-8'))
    return {}

def send_message(host: str, msg: str) -> bool:
    url = f"http://{host}/api/send"
    data = urllib.parse.urlencode({'msg': msg}).encode('utf-8')
    req = urllib.request.Request(url, data=data, headers={'Content-Type': 'application/x-www-form-urlencoded'})
    try:
        with urllib.request.urlopen(req, timeout=2.0) as resp:
            return resp.status == 200
    except Exception as e:
        print(f"{RED}Error sending: {e}{RESET}")
        return False

def render_leds(recent_hops):
    if not recent_hops:
        return ""
    
    lines = []
    num_hops = len(recent_hops)
    cols = 10
    rows = (num_hops + cols - 1) // cols
    for row in range(rows):
        row_str = "  "
        for col in range(cols):
            idx = row * cols + col
            if idx >= num_hops:
                break
            item = recent_hops[idx]
            ch = item.get("ch", 0)
            ok = item.get("ok", 0)
            if ok:
                row_str += f"{GREEN}[CH {ch:02d}]{RESET} "
            else:
                row_str += f"{RED}[CH {ch:02d}]{RESET} "
        lines.append(row_str)
    return "\n".join(lines)

def main():
    parser = argparse.ArgumentParser(description="HopperNet Web API Live Verifier")
    parser.add_argument("--host", default="192.168.4.1", help="ESP32 Gateway IP (default: 192.168.4.1)")
    parser.add_argument("--send", type=str, default="", help="Send test message to node")
    args = parser.parse_args()

    if args.send:
        print(f"\n{CYAN}Transmitting message to http://{args.host}/api/send: \"{args.send}\"...{RESET}")
        if send_message(args.host, args.send):
            print(f"{GREEN}✓ Message successfully queued for FHSS transmission!{RESET}\n")
        else:
            print(f"{RED}✗ Failed to send message.{RESET}\n")
        return

    print(f"\n{BOLD}{CYAN}Connecting to HopperNet Node at http://{args.host}...{RESET}\n")

    try:
        while True:
            try:
                d = fetch_status(args.host)
            except Exception as e:
                print(f"\033[H\033[J{RED}Waiting for connection to http://{args.host}... ({e}){RESET}")
                time.sleep(1.0)
                continue

            node_name = d.get("node", "unknown").upper()
            synced = d.get("synced", False)
            cur_ch = d.get("ch", 0)
            cur_hop = d.get("hop", 0)
            matched = d.get("matched_sec", 0)
            hops_sec = d.get("hops_per_sec", 40)
            recent_hops = d.get("recent_hops", [])
            pct = int((matched / hops_sec) * 100) if hops_sec else 0

            # Clear screen
            print("\033[H\033[J", end="")

            sync_badge = f"{GREEN}{BOLD}LOCKED{RESET}" if synced or node_name == "NODE_B" else f"{YELLOW}SCANNING{RESET}"
            rate_badge = f"{GREEN}{matched}/{hops_sec} ({pct}%){RESET}" if pct >= 75 else f"{RED}{matched}/{hops_sec} ({pct}%){RESET}"

            print(f"{BOLD}{CYAN}========================================================================{RESET}")
            print(f"{BOLD}{CYAN}  HOPPERNET LIVE WEB VERIFIER — {node_name}                          {RESET}")
            print(f"{BOLD}{CYAN}========================================================================{RESET}")
            print(f"  {BOLD}Status:{RESET} {sync_badge}  |  {BOLD}Current Channel:{RESET} CH {cur_ch} ({2400 + cur_ch} MHz)  |  {BOLD}Master Hop:{RESET} #{cur_hop}")
            print(f"  {BOLD}Sync Quality (Past 1s):{RESET} {rate_badge}")
            print("-" * 72)
            print(f"\n  {BOLD}40-HOP SPECTRUM STREAM (Chronological 1-Sec Window):{RESET}\n")
            print(render_leds(recent_hops))
            print("\n" + "-" * 72)

            history = d.get("history", [])
            if history:
                print(f"  {BOLD}Recent Messages ({len(history)}):{RESET}")
                for m in history[-5:]:
                    seq = m.get("seq", 0)
                    hop = m.get("hop", 0)
                    text = m.get("text", "")
                    print(f"    {DIM}#{seq} (Hop {hop}):{RESET} {BOLD}{text}{RESET}")
            else:
                print(f"  {DIM}No recent messages.{RESET}")

            print(f"\n{DIM}Press Ctrl+C to stop. Refresh rate: 500ms.{RESET}\n")
            time.sleep(0.5)

    except KeyboardInterrupt:
        print(f"\n{GREEN}Verifier stopped.{RESET}\n")

if __name__ == "__main__":
    main()
