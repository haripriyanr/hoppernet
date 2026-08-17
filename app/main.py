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
    if "NODE A" in line_upper:
        node_conn.detected_role = "NODE_A"
    elif "NODE B" in line_upper:
        node_conn.detected_role = "NODE_B"
    elif "NODE C" in line_upper:
        node_conn.detected_role = "NODE_C"
    elif "JAMMER" in line_upper:
        node_conn.detected_role = "JAMMER"

    if "SYNC ACQUIRED" in line_upper:
        node_conn.stats["synced"] = True

    # Parse common fields
    if "CH:" in line_upper or "CH=" in line_upper:
        try:
            parts = line.replace(":", " ").replace("=", " ").split()
            for i, p in enumerate(parts):
                if p.upper() == "CH" and i + 1 < len(parts):
                    node_conn.stats["channel"] = int(parts[i + 1])
                elif p.upper() == "HOP" and i + 1 < len(parts):
                    node_conn.stats["hop"] = int(parts[i + 1])
                elif p.upper() in ["BUFFER", "F", "BUF"] and i + 1 < len(parts):
                    node_conn.stats["buffer_depth"] = int(parts[i + 1])
                elif p.upper() in ["JAM", "BLACKLIST"] and i + 1 < len(parts):
                    node_conn.stats["blacklist_count"] = int(parts[i + 1])
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

    entry = {
        "id": int(time.time() * 1000),
        "source": req.source,
        "target": req.target,
        "content": req.content,
        "status": "dispatched" if target_node else "queued_cloud",
        "ts": time.strftime("%H:%M:%S")
    }
    chat_history.append(entry)

    # Broadcast to web app
    await broadcast_ws({"type": "chat_message", "message": entry})
    return {"status": "ok", "entry": entry}

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
