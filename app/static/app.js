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

function updateNodeCard(port, role, stats) {
    if (role === "NODE_A") {
        document.getElementById("box-node-a").classList.add("connected");
        document.getElementById("port-node-a").textContent = port;
        if (stats.sent) document.getElementById("sent-node-a").textContent = stats.sent;
        if (stats.received) document.getElementById("recv-node-a").textContent = stats.received;
        document.getElementById("sync-node-a").textContent = stats.synced ? "SYNCED" : "SCANNING";
    } else if (role === "NODE_B") {
        document.getElementById("box-node-b").classList.add("connected");
        document.getElementById("port-node-b").textContent = port;
        if (stats.buffer_depth !== undefined) document.getElementById("fbuf-node-b").textContent = stats.buffer_depth;
        if (stats.blacklist_count !== undefined) document.getElementById("jam-node-b").textContent = stats.blacklist_count;
    } else if (role === "NODE_C") {
        document.getElementById("box-node-c").classList.add("connected");
        document.getElementById("port-node-c").textContent = port;
        if (stats.received) document.getElementById("recv-node-c").textContent = stats.received;
        document.getElementById("sync-node-c").textContent = stats.synced ? "SYNCED" : "SCANNING";
    } else if (role === "JAMMER") {
        document.getElementById("box-jammer").classList.add("connected");
        document.getElementById("port-jammer").textContent = port;
        document.getElementById("state-jammer").textContent = "ACTIVE";
        if (stats.channel) document.getElementById("ch-jammer").textContent = `CH ${stats.channel}`;
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
