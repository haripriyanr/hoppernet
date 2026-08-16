// MedRelay / HopperNet Dashboard Engine
// Real-time Supabase Realtime WebSocket client + 124-channel spectrum visualizer

const SUPABASE_URL = "https://pbebctpsswuesmgkmecn.supabase.co";
const SUPABASE_ANON_KEY = "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJpc3MiOiJzdXBhYmFzZSIsInJlZiI6InBiZWJjdHBzc3d1ZXNtZ2ttZWNuIiwicm9sZSI6ImFub24iLCJpYXQiOjE3ODY5MDE0NzksImV4cCI6MjEwMjQ3NzQ3OX0.NZT-FFmEjprHTlcYeAYCkI5yCY5wbhW7iP9H0_AlxWk";

const supabase = window.supabase.createClient(SUPABASE_URL, SUPABASE_ANON_KEY);

const NUM_CHANNELS = 124;
const CHANNEL_BASE = 2;

// Blacklist State & Active Channel
let blacklistedChannels = new Set();
let activeHopChannel = null;
let receivedCount = 0;
let eventsCount = 0;

// DOM Elements
const spectrumGrid = document.getElementById("spectrum-grid");
const feedContainer = document.getElementById("feed-container");
const eventsList = document.getElementById("events-list");
const msgForm = document.getElementById("msg-form");
const msgInput = document.getElementById("msg-input");
const feedCountBadge = document.getElementById("feed-count");
const eventsCountBadge = document.getElementById("events-count");

// Initialize 124 Channel Grid
function initSpectrumGrid() {
    spectrumGrid.innerHTML = "";
    for (let ch = CHANNEL_BASE; ch < CHANNEL_BASE + NUM_CHANNELS; ch++) {
        const block = document.createElement("div");
        block.className = "ch-block";
        block.id = `ch-${ch}`;
        block.textContent = ch;
        block.title = `Channel ${ch} (${(2400 + ch)} MHz)`;
        spectrumGrid.appendChild(block);
    }
}

// Update Active Hop Channel Highlight
function setActiveChannel(ch) {
    if (activeHopChannel && activeHopChannel !== ch) {
        const oldBlock = document.getElementById(`ch-${activeHopChannel}`);
        if (oldBlock) oldBlock.classList.remove("active-hop");
    }
    activeHopChannel = ch;
    const newBlock = document.getElementById(`ch-${ch}`);
    if (newBlock) {
        newBlock.classList.add("active-hop");
    }
}

// Update Blacklisted Channel in Grid
function markChannelJammed(ch, isJammed) {
    const block = document.getElementById(`ch-${ch}`);
    if (!block) return;
    if (isJammed) {
        blacklistedChannels.add(ch);
        block.classList.add("jammed");
    } else {
        blacklistedChannels.delete(ch);
        block.classList.remove("jammed");
    }
    document.getElementById("stat-blacklisted-count").textContent = blacklistedChannels.size;
    document.getElementById("node-b-jammed").textContent = blacklistedChannels.size;
}

// Render Inbound Received Message from Node C
function addReceivedMessage(msg) {
    if (feedContainer.querySelector(".empty-state")) {
        feedContainer.innerHTML = "";
    }
    receivedCount++;
    feedCountBadge.textContent = `${receivedCount} received`;

    const item = document.createElement("div");
    item.className = "feed-item";
    const timeStr = new Date(msg.received_at || Date.now()).toLocaleTimeString();

    item.innerHTML = `
        <div class="feed-item-header">
            <span>SEQ #${msg.seq ?? "?"} | HOP #${msg.hop ?? "?"}</span>
            <span>${timeStr}</span>
        </div>
        <div class="feed-item-content">${escapeHtml(msg.content)}</div>
        <div class="feed-item-footer">
            <span class="tag">RF CH ${msg.channel ?? "?"}</span>
            <span class="tag">Node A ➔ B ➔ C</span>
            <span class="tag" style="color:#10b981;">✓ DELIVERED</span>
        </div>
    `;
    feedContainer.prepend(item);
}

// Render Blacklist / Interference Event
function addBlacklistEvent(evt) {
    if (eventsList.querySelector(".empty-state")) {
        eventsList.innerHTML = "";
    }
    eventsCount++;
    eventsCountBadge.textContent = `${eventsCount} events`;

    const row = document.createElement("div");
    row.className = "event-row";
    const timeStr = new Date(evt.ts || Date.now()).toLocaleTimeString();

    row.innerHTML = `
        <span>⚠️ Channel ${evt.channel} BLACKLISTED</span>
        <span>${escapeHtml(evt.reason || "RPD carrier threshold")} | ${timeStr}</span>
    `;
    eventsList.prepend(row);
    markChannelJammed(evt.channel, evt.action === "blacklisted");
}

// Handle Telemetry Update from any Node
function handleTelemetry(t) {
    if (t.current_channel) {
        setActiveChannel(t.current_channel);
    }

    if (t.node === "node_a") {
        document.getElementById("node-a-sent").textContent = t.sent;
        document.getElementById("node-a-acked").textContent = t.acked;
        document.getElementById("node-a-hop").textContent = t.current_hop;
        const chip = document.getElementById("chip-node-a");
        chip.textContent = t.synced ? "SYNCED" : "SCANNING";
        chip.className = t.synced ? "node-chip active" : "node-chip";
    } else if (t.node === "node_b") {
        document.getElementById("node-b-buffer").textContent = `${t.buffer_depth} pkts`;
        document.getElementById("stat-buffer-depth").textContent = t.buffer_depth;
        document.getElementById("node-b-relayed").textContent = t.sent;
        document.getElementById("node-b-jammed").textContent = t.blacklist_count;
        document.getElementById("stat-blacklisted-count").textContent = t.blacklist_count;
    } else if (t.node === "node_c") {
        document.getElementById("node-c-received").textContent = t.received;
        const chip = document.getElementById("chip-node-c");
        chip.textContent = t.synced ? "SYNCED" : "SCANNING";
        chip.className = t.synced ? "node-chip active" : "node-chip";
    }
}

// Helper: Escape HTML
function escapeHtml(text) {
    const div = document.createElement("div");
    div.textContent = text;
    return div.innerHTML;
}

// Dispatch Outbound Message to Supabase (Node A pulls it)
msgForm.addEventListener("submit", async (e) => {
    e.preventDefault();
    const content = msgInput.value.trim();
    if (!content) return;

    msgInput.disabled = true;
    const sendBtn = document.getElementById("send-btn");
    sendBtn.innerHTML = "<span>SENDING...</span>";

    try {
        const { data, error } = await supabase
            .from("messages")
            .insert([{ content: content, status: "pending", sender_name: "Dispatcher Phone" }]);

        if (error) throw error;
        msgInput.value = "";
    } catch (err) {
        alert("Error sending message: " + err.message);
    } finally {
        msgInput.disabled = false;
        sendBtn.innerHTML = `<span>DISPATCH</span><svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="m22 2-7 20-4-9-9-4Z"/><path d="M22 2 11 13"/></svg>`;
        msgInput.focus();
    }
});

// Preset Buttons Handler
document.querySelectorAll(".preset-btn").forEach(btn => {
    btn.addEventListener("click", () => {
        msgInput.value = btn.getAttribute("data-msg");
        msgInput.focus();
    });
});

// Load Initial Data
async function loadInitialData() {
    // 1. Initial Received Messages
    const { data: recvs } = await supabase
        .from("received_messages")
        .select("*")
        .order("received_at", { ascending: false })
        .limit(20);

    if (recvs && recvs.length > 0) {
        recvs.reverse().forEach(addReceivedMessage);
    }

    // 2. Initial Blacklist Events
    const { data: events } = await supabase
        .from("blacklist_events")
        .select("*")
        .order("ts", { ascending: false })
        .limit(10);

    if (events && events.length > 0) {
        events.forEach(addBlacklistEvent);
    }

    // 3. Initial Telemetry
    const { data: telemetry } = await supabase
        .from("telemetry")
        .select("*")
        .order("ts", { ascending: false })
        .limit(3);

    if (telemetry) {
        telemetry.forEach(handleTelemetry);
    }
}

// Subscribe to Supabase Realtime Broadcasts
function subscribeRealtime() {
    supabase
        .channel("hoppernet-live")
        .on("postgres_changes", { event: "INSERT", schema: "public", table: "received_messages" }, payload => {
            addReceivedMessage(payload.new);
        })
        .on("postgres_changes", { event: "INSERT", schema: "public", table: "blacklist_events" }, payload => {
            addBlacklistEvent(payload.new);
        })
        .on("postgres_changes", { event: "INSERT", schema: "public", table: "telemetry" }, payload => {
            handleTelemetry(payload.new);
        })
        .subscribe((status) => {
            const dot = document.getElementById("cloud-status-dot");
            const text = document.getElementById("cloud-status-text");
            if (status === "SUBSCRIBED") {
                dot.style.background = "#10b981";
                text.textContent = "Cloud Sync Active";
            } else {
                dot.style.background = "#f59e0b";
                text.textContent = `Sync (${status})`;
            }
        });
}

// Boot
window.addEventListener("DOMContentLoaded", () => {
    initSpectrumGrid();
    loadInitialData();
    subscribeRealtime();
});
