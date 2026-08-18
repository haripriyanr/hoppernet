import asyncio
import json
import time
from typing import Optional, Dict, Any
import serial
import serial.tools.list_ports
from fastapi import FastAPI, WebSocket, WebSocketDisconnect, HTTPException
from fastapi.middleware.cors import CORSMiddleware
from fastapi.staticfiles import StaticFiles
from fastapi.responses import HTMLResponse, FileResponse
import uvicorn
from pydantic import BaseModel

app = FastAPI(title="HopperNet / MedRelay Hub")

app.add_middleware(
    CORSMiddleware,
    allow_origins=["*"],
    allow_credentials=True,
    allow_methods=["*"],
    allow_headers=["*"],
)

# ---------------- In-Memory State & Serial Hub ----------------
class NodeConnection:
    def __init__(self, port: str, baud: int = 115200):
        self.port = port
        self.baud = baud
        self.ser: Optional[serial.Serial] = None
        self.detected_role: str = "UNKNOWN"  # "NODE_A", "NODE_B", "NODE_C", "JAMMER"
        self.connected: bool = False
        self.last_seen: float = time.time()
        self.stats: Dict[str, Any] = {
            "sent": 0,
            "acked": 0,
            "received": 0,
            "channel": 0,
            "hop": 0,
            "buffer_depth": 0,
            "blacklist_count": 0,
            "synced": False
        }

active_nodes: Dict[str, NodeConnection] = {}
ws_clients = set()
chat_history = []
events_history = []

async def broadcast_ws(payload: dict):
    dead_clients = set()
    for ws in ws_clients:
        try:
            await ws.send_json(payload)
        except Exception:
            dead_clients.add(ws)
    for ws in dead_clients:
        ws_clients.remove(ws)

# ---------------- Background Auto-Discovery & Serial Monitor ----------------
def parse_line(node_conn: NodeConnection, line: str):
    node_conn.last_seen = time.time()
    line_upper = line.upper()

    # Auto-detection signatures
    if "NODE A" in line_upper or "NODE_A" in line_upper:
        node_conn.detected_role = "NODE_A"
    elif "NODE B" in line_upper or "NODE_B" in line_upper:
        node_conn.detected_role = "NODE_B"
    elif "NODE C" in line_upper or "NODE_C" in line_upper:
        node_conn.detected_role = "NODE_C"
    elif "JAMMER" in line_upper:
        node_conn.detected_role = "JAMMER"

    if "SYNC ACQUIRED" in line_upper or "SYNC=LOCKED" in line_upper:
        node_conn.stats["synced"] = True
    elif "SYNC=SCAN" in line_upper or "SCANNING" in line_upper:
        node_conn.stats["synced"] = False

    if "SIM LINK DOWN" in line_upper:
        node_conn.stats["link_down"] = True
    elif "LINK RESTORED" in line_upper:
        node_conn.stats["link_down"] = False

    # Extract structured key-value metrics (TELEMETRY / HANDSHAKE / STATUS)
    # Format: KEY=VAL or KEY:VAL
    try:
        norm = line.replace("|", " ").replace(",", " ")
        for token in norm.split():
            if "=" in token:
                k, v = token.split("=", 1)
                k = k.upper()
                if k == "CH":
                    node_conn.stats["channel"] = int(v)
                elif k in ["SENT", "STATS_SENT"]:
                    node_conn.stats["sent"] = int(v)
                elif k in ["RECV", "RECEIVED"]:
                    node_conn.stats["received"] = int(v)
                elif k in ["DELIVERED", "DEL"]:
                    node_conn.stats["delivered"] = int(v)
                elif k in ["CUSTODY", "ACK"]:
                    node_conn.stats["acked"] = int(v)
                elif k in ["Q", "FWD_BUF", "BUFFER_DEPTH", "BUF"]:
                    node_conn.stats["buffer_depth"] = int(v)
                elif k in ["JAM", "BLACKLIST", "BL"]:
                    node_conn.stats["blacklist_count"] = int(v)
                elif k == "SF":
                    node_conn.stats["hop"] = int(v)
                elif k == "PCT":
                    node_conn.stats["sync_pct"] = float(v)
                elif k == "RPD":
                    node_conn.stats["rpd"] = v  # "CLEAN" or "HIGH"
                elif k == "LOSS":
                    node_conn.stats["loss_pct"] = v  # e.g. "0.0%"
                elif k == "DRIFT":
                    node_conn.stats["drift_us"] = v
                elif k == "AGE":
                    node_conn.stats["beacon_age_ms"] = v
                elif k == "RTT":
                    node_conn.stats["rtt_us"] = v
    except Exception:
        pass

    return {
        "type": "log",
        "port": node_conn.port,
        "role": node_conn.detected_role,
        "line": line,
        "ts": time.time(),
        "stats": node_conn.stats
    }

async def serial_poller_task():
    while True:
        # Scan system COM ports
        ports = serial.tools.list_ports.comports()
        avail_ports = {p.device for p in ports}

        # Remove disconnected
        for port in list(active_nodes.keys()):
            if port not in avail_ports:
                node = active_nodes.pop(port)
                if node.ser:
                    try:
                        node.ser.close()
                    except Exception:
                        pass
                await broadcast_ws({"type": "node_disconnected", "port": port})

        # Connect newly plugged devices
        for p in ports:
            if p.device not in active_nodes:
                try:
                    s = serial.Serial(p.device, 115200, timeout=0.1)
                    node = NodeConnection(p.device, 115200)
                    node.ser = s
                    node.connected = True
                    active_nodes[p.device] = node
                    await broadcast_ws({"type": "node_connected", "port": p.device})
                except Exception:
                    pass

        # Read available lines
        for port, node in list(active_nodes.items()):
            if node.ser and node.ser.is_open:
                try:
                    while node.ser.in_waiting:
                        raw = node.ser.readline()
                        line = raw.decode("utf-8", errors="replace").strip()
                        if line:
                            evt = parse_line(node, line)
                            await broadcast_ws(evt)
                except Exception:
                    pass

        await asyncio.sleep(0.05)

@app.on_event("startup")
async def startup_event():
    asyncio.create_task(serial_poller_task())

# ---------------- API Models & Routes ----------------
class SendMessageRequest(BaseModel):
    source: str  # "NODE_A" or "NODE_C"
    target: str  # "NODE_C" or "NODE_A"
    content: str

@app.get("/api/nodes")
def get_nodes():
    return [
        {
            "port": n.port,
            "role": n.detected_role,
            "stats": n.stats,
            "last_seen": n.last_seen
        }
        for n in active_nodes.values()
    ]

@app.post("/api/send")
async def send_message(req: SendMessageRequest):
    # Find matching port for requested source node
    target_node = None
    for n in active_nodes.values():
        if n.detected_role == req.source and n.ser and n.ser.is_open:
            target_node = n
            break

    dispatched = False
    if target_node and target_node.ser and target_node.ser.is_open:
        try:
            target_node.ser.write(f"SEND:{req.content}\n".encode("utf-8"))
            dispatched = True
        except Exception as e:
            print(f"Error sending serial to {req.source}: {e}")

    entry = {
        "id": int(time.time() * 1000),
        "source": req.source,
        "target": req.target,
        "content": req.content,
        "status": "transmitted_rf" if dispatched else "device_not_connected",
        "ts": time.strftime("%H:%M:%S")
    }
    chat_history.append(entry)

    # Broadcast to web app
    await broadcast_ws({"type": "chat_message", "message": entry})
    return {"status": "ok", "entry": entry}

@app.post("/api/node_c/linkdown")
async def toggle_node_c_linkdown():
    # Find Node C connection
    node_c = None
    for n in active_nodes.values():
        if n.detected_role == "NODE_C" and n.ser and n.ser.is_open:
            node_c = n
            break

    if not node_c:
        raise HTTPException(status_code=404, detail="Node C device not connected via Serial")

    current_state = node_c.stats.get("link_down", False)
    new_state = not current_state
    node_c.stats["link_down"] = new_state

    try:
        node_c.ser.write(b"CMD:LINKDOWN\n")
    except Exception as e:
        print(f"Error sending linkdown command to Node C: {e}")

    await broadcast_ws({
        "type": "link_down_toggle",
        "role": "NODE_C",
        "link_down": new_state
    })

    return {
        "status": "ok",
        "node": "NODE_C",
        "link_down": new_state,
        "message": "Node C is now RF-SILENT (Node B buffering)" if new_state else "Node C is now ONLINE (Node B flushing)"
    }

@app.websocket("/ws")
async def websocket_endpoint(websocket: WebSocket):
    await websocket.accept()
    ws_clients.add(websocket)
    # Send initial snapshot
    await websocket.send_json({
        "type": "init",
        "nodes": [
            {"port": n.port, "role": n.detected_role, "stats": n.stats}
            for n in active_nodes.values()
        ],
        "history": chat_history[-30:]
    })
    try:
        while True:
            await websocket.receive_text()
    except WebSocketDisconnect:
        if websocket in ws_clients:
            ws_clients.remove(websocket)

# Serve App Frontend
app.mount("/", StaticFiles(directory="app/static", html=True), name="static")

if __name__ == "__main__":
    uvicorn.run("main:app", host="0.0.0.0", port=8000, reload=False)
