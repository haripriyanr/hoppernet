// HopperNet / MedRelay Auto-Hub Client

const wsProtocol = window.location.protocol === "https:" ? "wss:" : "ws:";
const wsUrl = `${wsProtocol}//${window.location.host}/ws`;
let socket = null;

const hubStatus = document.getElementById("hub-status");
const chatStream = document.getElementById("chat-stream");
const logStream = document.getElementById("log-stream");
const chatInput = document.getElementById("chat-input");
const btnSend = document.getElementById("btn-send");
const chatDirection = document.getElementById("chat-direction");

function connectWebSocket() {
    socket = new WebSocket(wsUrl);

    socket.onopen = () => {
        hubStatus.textContent = "CONNECTED (AUTO-SCANNING)";
        hubStatus.style.borderColor = "#10b981";
        hubStatus.style.color = "#10b981";
    };

    socket.onmessage = (event) => {
        const data = JSON.parse(event.data);
        handleServerEvent(data);
    };

    socket.onclose = () => {
        hubStatus.textContent = "DISCONNECTED (RECONNECTING...)";
        hubStatus.style.borderColor = "#ef4444";
        hubStatus.style.color = "#ef4444";
        setTimeout(connectWebSocket, 2000);
    };
}

function handleServerEvent(evt) {
    if (evt.type === "log") {
        appendLog(evt);
        updateNodeCard(evt.port, evt.role, evt.stats);
    } else if (evt.type === "chat_message") {
        appendChat(evt.message);
    } else if (evt.type === "init") {
        if (evt.history) {
            evt.history.forEach(appendChat);
        }
    }
}

function appendLog(log) {
    const entry = document.createElement("div");
    entry.className = "log-entry";
    const timeStr = new Date(log.ts * 1000).toLocaleTimeString();
    entry.innerHTML = `<span style="color:#64748b;">[${timeStr}]</span> <span class="port">[${log.port}]</span> <span class="role">[${log.role}]</span> ${escapeHtml(log.line)}`;
    logStream.appendChild(entry);
    logStream.scrollTop = logStream.scrollHeight;

    // Live Handshake RTT measurement update
    if (log.line && log.line.includes("RTT=")) {
        const m = log.line.match(/RTT=(\d+)_us/);
        if (m && m[1]) {
            const badge = document.getElementById("rtt-badge");
            if (badge) badge.textContent = `${m[1]} \u03BCs Handshake RTT`;
        }
    }
}

function appendChat(msg) {
    const bubble = document.createElement("div");
    const isOutbound = msg.source === "NODE_A";
    bubble.className = `chat-bubble ${isOutbound ? "outbound" : "inbound"}`;
    bubble.innerHTML = `
        <div class="bubble-meta">
            <span><strong>${msg.source}</strong> ➔ <strong>${msg.target}</strong></span>
            <span>${msg.ts || new Date().toLocaleTimeString()}</span>
        </div>
        <div class="bubble-content">${escapeHtml(msg.content)}</div>
    `;
    chatStream.appendChild(bubble);
    chatStream.scrollTop = chatStream.scrollHeight;
}

let isNodeCDown = false;
const btnLinkDown = document.getElementById("btn-desktop-linkdown");

function updateLinkDownUI(down) {
    isNodeCDown = down;
    if (btnLinkDown) {
        if (down) {
            btnLinkDown.style.background = "linear-gradient(180deg, #059669, #047857)";
            btnLinkDown.textContent = "🟢 RESTORE NODE C (FLUSH SRAM BUFFER)";
            document.getElementById("sync-node-c").textContent = "SIM DEAD-ZONE";
            document.getElementById("sync-node-c").style.color = "#f87171";
        } else {
            btnLinkDown.style.background = "linear-gradient(180deg, #dc2626, #991b1b)";
            btnLinkDown.textContent = "🔌 SIMULATE DEAD-ZONE (TURN NODE C OFF)";
            document.getElementById("sync-node-c").style.color = "#34d399";
        }
    }
}

if (btnLinkDown) {
    btnLinkDown.addEventListener("click", async () => {
        try {
            const res = await fetch("/api/node_c/linkdown", { method: "POST" });
            const data = await res.json();
            updateLinkDownUI(data.link_down);
        } catch (err) {
            console.error("Linkdown toggle error:", err);
        }
    });
}

function updateNodeCard(port, role, stats) {
    if (role === "NODE_A") {
        document.getElementById("box-node-a").classList.add("connected");
        document.getElementById("port-node-a").textContent = port;
        if (stats.sent !== undefined) document.getElementById("sent-node-a").textContent = `${stats.sent} sent (${stats.acked || stats.sent} ack)`;
        const syncText = stats.sync_pct !== undefined ? `${stats.sync_pct}% (${stats.synced ? "LOCKED" : "SCAN"})` : (stats.synced ? "100.0% (LOCKED)" : "SCANNING");
        document.getElementById("sync-node-a").textContent = syncText;
        if (stats.rpd) {
            const el = document.getElementById("rpd-node-a");
            el.textContent = stats.rpd === "HIGH" ? "> -64dBm (High RF)" : "< -64dBm (Clean)";
            el.className = stats.rpd === "HIGH" ? "val danger" : "val ok";
        }
        if (stats.loss_pct !== undefined) {
            const el = document.getElementById("loss-node-a");
            el.textContent = `${stats.loss_pct} (${(stats.sent || 0) - (stats.acked || stats.sent || 0)} drops)`;
            el.className = parseFloat(stats.loss_pct) > 0 ? "val danger" : "val ok";
        }
    } else if (role === "NODE_B") {
        document.getElementById("box-node-b").classList.add("connected");
        document.getElementById("port-node-b").textContent = port;
        if (stats.buffer_depth !== undefined) {
            document.getElementById("fbuf-node-b").textContent = `${stats.buffer_depth} pkts`;
            const highlight = document.getElementById("fbuf-highlight");
            if (highlight) highlight.textContent = stats.buffer_depth;
        }
        if (stats.blacklist_count !== undefined) document.getElementById("jam-node-b").textContent = `${stats.blacklist_count} / 124`;
        if (stats.rpd) {
            const el = document.getElementById("rpd-node-b");
            el.textContent = stats.rpd === "HIGH" ? "> -64dBm (High RF)" : "< -64dBm (Clean)";
            el.className = stats.rpd === "HIGH" ? "val danger" : "val ok";
        }
        if (stats.loss_pct !== undefined) {
            const el = document.getElementById("loss-node-b");
            el.textContent = `${stats.loss_pct} (0 drops)`;
            el.className = parseFloat(stats.loss_pct) > 0 ? "val danger" : "val ok";
        }
    } else if (role === "NODE_C") {
        document.getElementById("box-node-c").classList.add("connected");
        document.getElementById("port-node-c").textContent = port;
        if (stats.received !== undefined) document.getElementById("recv-node-c").textContent = `${stats.received} pkts`;
        if (stats.link_down !== undefined) {
            updateLinkDownUI(stats.link_down);
        } else {
            const syncText = stats.sync_pct !== undefined ? `${stats.sync_pct}% (${stats.synced ? "LOCKED" : "SCAN"})` : (stats.synced ? "100.0% (LOCKED)" : "SCANNING");
            document.getElementById("sync-node-c").textContent = syncText;
        }
        if (stats.rpd) {
            const el = document.getElementById("rpd-node-c");
            el.textContent = stats.rpd === "HIGH" ? "> -64dBm (High RF)" : "< -64dBm (Clean)";
            el.className = stats.rpd === "HIGH" ? "val danger" : "val ok";
        }
        if (stats.loss_pct !== undefined) {
            const el = document.getElementById("loss-node-c");
            el.textContent = `${stats.loss_pct} (${(stats.sent || 0) - (stats.acked || stats.sent || 0)} drops)`;
            el.className = parseFloat(stats.loss_pct) > 0 ? "val danger" : "val ok";
        }
    } else if (role === "JAMMER") {
        document.getElementById("box-jammer").classList.add("connected");
        document.getElementById("port-jammer").textContent = port;
        document.getElementById("state-jammer").textContent = "ACTIVE";
        if (stats.channel) document.getElementById("ch-jammer").textContent = `CH ${stats.channel} (${2400 + stats.channel} MHz)`;
    }
}

function escapeHtml(text) {
    const div = document.createElement("div");
    div.textContent = text;
    return div.innerHTML;
}

btnSend.addEventListener("click", sendMessage);
chatInput.addEventListener("keypress", (e) => {
    if (e.key === "Enter") sendMessage();
});

async function sendMessage() {
    const content = chatInput.value.trim();
    if (!content) return;

    const dir = chatDirection.value;
    const src = dir === "A_TO_C" ? "NODE_A" : "NODE_C";
    const dst = dir === "A_TO_C" ? "NODE_C" : "NODE_A";

    chatInput.value = "";
    try {
        await fetch("/api/send", {
            method: "POST",
            headers: { "Content-Type": "application/json" },
            body: JSON.stringify({ source: src, target: dst, content: content })
        });
    } catch (err) {
        console.error("Send error:", err);
    }
}

window.addEventListener("DOMContentLoaded", connectWebSocket);
