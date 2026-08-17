"""
HopperNet Presentation Generator — clean student-style deck.
Purple background, white text, no file references, no meta-documentation.
Talks only about the system itself.
"""

from pptx import Presentation
from pptx.util import Inches, Pt
from pptx.dml.color import RGBColor
from pptx.enum.text import PP_ALIGN
from pptx.enum.shapes import MSO_SHAPE

# ── Palette ──
BG      = RGBColor(0x1a, 0x0a, 0x3c)
CARD    = RGBColor(0x2a, 0x14, 0x5e)
ACCENT  = RGBColor(0xa0, 0x60, 0xff)
CYAN    = RGBColor(0x38, 0xbd, 0xf8)
GREEN   = RGBColor(0x34, 0xd3, 0x99)
AMBER   = RGBColor(0xfb, 0xbf, 0x24)
RED     = RGBColor(0xf8, 0x71, 0x71)
WHITE   = RGBColor(0xff, 0xff, 0xff)
GREY    = RGBColor(0xb0, 0xa0, 0xd0)
DIVIDER = RGBColor(0x50, 0x30, 0x90)

W = Inches(13.333)
H = Inches(7.5)

def new_prs():
    prs = Presentation()
    prs.slide_width = W
    prs.slide_height = H
    return prs

def blank(prs):
    return prs.slides.add_slide(prs.slide_layouts[6])

def rect(slide, l, t, w, h, fill, line_color=None, line_pt=0):
    s = slide.shapes.add_shape(MSO_SHAPE.RECTANGLE, l, t, w, h)
    s.fill.solid()
    s.fill.fore_color.rgb = fill
    if line_color:
        s.line.color.rgb = line_color
        s.line.width = Pt(line_pt)
    else:
        s.line.fill.background()
    return s

def tb(slide, l, t, w, h, text, size, color=WHITE, bold=False, italic=False,
       align=PP_ALIGN.LEFT, wrap=True):
    box = slide.shapes.add_textbox(l, t, w, h)
    tf = box.text_frame
    tf.word_wrap = wrap
    tf.margin_left = tf.margin_right = tf.margin_top = tf.margin_bottom = 0
    p = tf.paragraphs[0]
    p.alignment = align
    run = p.add_run()
    run.text = text
    run.font.size = Pt(size)
    run.font.color.rgb = color
    run.font.bold = bold
    run.font.italic = italic
    return tf

def hline(slide, l, t, w, color, thickness_pt=0.75):
    s = slide.shapes.add_shape(MSO_SHAPE.RECTANGLE, l, t, w, Pt(thickness_pt))
    s.fill.solid()
    s.fill.fore_color.rgb = color
    s.line.fill.background()

def bgfill(slide, prs):
    rect(slide, 0, 0, prs.slide_width, prs.slide_height, BG)

def header(slide, eyebrow, title, sub=""):
    hline(slide, Inches(0.7), Inches(0.48), Inches(11.93), ACCENT, 2.5)
    tb(slide, Inches(0.7), Inches(0.15), Inches(11.93), Inches(0.35),
       eyebrow.upper(), 9.5, ACCENT, bold=True)
    tb(slide, Inches(0.7), Inches(0.56), Inches(11.93), Inches(0.72),
       title, 22, WHITE, bold=True)
    if sub:
        tb(slide, Inches(0.7), Inches(1.25), Inches(11.93), Inches(0.4),
           sub, 11, GREY)

def kpi_strip(slide, items):
    n = len(items)
    col_w = Inches(11.93) / n
    for i, (val, lbl, color) in enumerate(items):
        l = Inches(0.7) + col_w * i
        rect(slide, l, Inches(6.5), col_w - Inches(0.1),
             Inches(0.72), CARD, DIVIDER, 0.6)
        tb(slide, l, Inches(6.52), col_w - Inches(0.1), Inches(0.35),
           val, 18, color, bold=True, align=PP_ALIGN.CENTER)
        tb(slide, l, Inches(6.88), col_w - Inches(0.1), Inches(0.28),
           lbl, 9, GREY, align=PP_ALIGN.CENTER)


# ==========================================================================
# SLIDE 1 — TITLE
# ==========================================================================
def slide_title(prs):
    s = blank(prs)
    bgfill(s, prs)
    rect(s, 0, 0, W, Inches(0.08), ACCENT)

    tb(s, Inches(0.9), Inches(0.8), Inches(11), Inches(0.4),
       "HopperNet  /  MedRelay", 12, CYAN, bold=True)
    tb(s, Inches(0.9), Inches(1.2), Inches(11), Inches(0.85),
       "Jammer-Resilient Slotted FHSS Mesh\nwith Dynamic Blacklisting & Persistent Edge Buffering",
       28, WHITE, bold=True)
    tb(s, Inches(0.9), Inches(2.15), Inches(11), Inches(0.35),
       "3-Node ESP32 Mesh  |  nRF24L01+ 2.4 GHz Transceiver  |  124 RF Channels  |  Real-Time Cloud Telemetry",
       11, GREY)

    hline(s, Inches(0.9), Inches(2.65), Inches(11.53), DIVIDER)

    # Left: project info
    tb(s, Inches(0.9), Inches(2.8), Inches(5.4), Inches(0.28),
       "PROJECT INFO", 9.5, ACCENT, bold=True)
    meta = [
        ("Domain",       "Embedded Systems, RF Communications, Wireless Security"),
        ("Architecture", "Node A (Source)  ->  Node B (Relay)  ->  Node C (Destination) + Jammer Console"),
        ("Transceiver",  "nRF24L01+  |  2.402 - 2.525 GHz  |  124 channels"),
        ("Backend",      "Supabase PostgreSQL with REST & WebSocket real-time sync"),
        ("Application",  "Emergency medical telemetry & tactical edge communications"),
    ]
    y = Inches(3.08)
    for lbl, val in meta:
        tb(s, Inches(0.9), y, Inches(1.1), Inches(0.22), f"{lbl}:", 9.5, GREY, bold=True)
        tb(s, Inches(2.05), y, Inches(4.2), Inches(0.22), val, 9.5, WHITE)
        y += Inches(0.26)

    # Right: rubric alignment
    tb(s, Inches(7.2), Inches(2.8), Inches(5.4), Inches(0.28),
       "EVALUATION RUBRIC", 9.5, ACCENT, bold=True)
    rubric = [
        ("2 marks", "Problem Statement & Objectives"),
        ("3 marks", "Literature Survey & Research Gap"),
        ("3 marks", "Proposed Methodology & Architecture"),
        ("3 marks", "Implementation Progress"),
        ("2 marks", "Documentation & Planning"),
        ("2 marks", "Team Presentation & Q&A"),
    ]
    y2 = Inches(3.08)
    for marks, desc in rubric:
        rect(s, Inches(7.2), y2 + Inches(0.04), Inches(0.82), Inches(0.18),
             CARD, DIVIDER, 0.5)
        tb(s, Inches(7.2), y2, Inches(0.82), Inches(0.22),
           marks, 8.5, GREEN, bold=True, align=PP_ALIGN.CENTER)
        tb(s, Inches(8.1), y2, Inches(4.5), Inches(0.22), desc, 9.5, WHITE)
        y2 += Inches(0.26)

    rect(s, 0, H - Inches(0.06), W, Inches(0.06), ACCENT)


# ==========================================================================
# SLIDE 2 — PROBLEM STATEMENT (Criteria 1, 2 marks)
# ==========================================================================
def slide_problem(prs):
    s = blank(prs)
    bgfill(s, prs)
    header(s,
           "Criteria 1  |  Problem Statement & Objectives  |  2 Marks",
           "Wireless Links That Fail When It Matters Most",
           "Critical communications break down under RF jamming and dead-zone disconnection")

    CW = Inches(3.6)
    CT = Inches(1.75)
    CH = Inches(4.5)
    GAP = Inches(0.27)
    cols = [Inches(0.7), Inches(0.7)+CW+GAP, Inches(0.7)+2*(CW+GAP)]

    # Col 1: Context
    rect(s, cols[0], CT, CW, CH, CARD, DIVIDER, 0.6)
    tb(s, cols[0]+Inches(0.18), CT+Inches(0.15), CW-Inches(0.36),
       Inches(0.26), "Operational Context", 11, RED, bold=True)
    hline(s, cols[0]+Inches(0.18), CT+Inches(0.45), CW-Inches(0.36), DIVIDER)
    items1 = [
        "The 2.4 GHz ISM band is heavily contested. Wi-Fi, Bluetooth, Zigbee, and cheap RF jammers all share the same spectrum.",
        "In hospital emergency triage (Code Blue alerts, ECG/SpO2 vitals), dropping even a single transmission can be fatal.",
        "A $10 software-defined radio can blind any standard single-channel wireless link indefinitely.",
        "Hospital elevators and concrete floors create sudden RF dead-zones where doctors lose reception mid-transmission.",
    ]
    y = CT + Inches(0.55)
    for line in items1:
        tb(s, cols[0]+Inches(0.18), y, CW-Inches(0.36), Inches(0.54),
           f"- {line}", 9.5, GREY, wrap=True)
        y += Inches(0.65)

    # Col 2: Core Problem
    rect(s, cols[1], CT, CW, CH, CARD, DIVIDER, 0.6)
    tb(s, cols[1]+Inches(0.18), CT+Inches(0.15), CW-Inches(0.36),
       Inches(0.26), "Core Engineering Problem", 11, AMBER, bold=True)
    hline(s, cols[1]+Inches(0.18), CT+Inches(0.45), CW-Inches(0.36), DIVIDER)
    items2 = [
        "Existing protocols lose clock synchronisation when their control channels are jammed, partitioning the entire network.",
        "Conventional mesh routers use drop-tail queuing: un-ACKed packets are discarded immediately, causing irreversible data loss.",
        "Re-negotiating frequency blacklists over RF adds 100-300 ms latency and the negotiation itself can be jammed.",
        "No existing commodity embedded system combines custody-transfer buffering with real-time cloud telemetry.",
    ]
    y = CT + Inches(0.55)
    for line in items2:
        tb(s, cols[1]+Inches(0.18), y, CW-Inches(0.36), Inches(0.54),
           f"- {line}", 9.5, GREY, wrap=True)
        y += Inches(0.65)

    # Col 3: Our Targets
    rect(s, cols[2], CT, CW, CH, CARD, DIVIDER, 0.6)
    tb(s, cols[2]+Inches(0.18), CT+Inches(0.15), CW-Inches(0.36),
       Inches(0.26), "Our Design Targets", 11, GREEN, bold=True)
    hline(s, cols[2]+Inches(0.18), CT+Inches(0.45), CW-Inches(0.36), DIVIDER)
    targets = [
        ("25 ms slotted FHSS", "Deterministic XORShift hops across 124 channels with <10 us jitter on FreeRTOS dual-core."),
        ("Zero-loss buffering", "SRAM + SPIFFS ring queue stores all packets during dead zones and drains on reconnect."),
        ("Zero-negotiation blacklisting", "RPD carrier scan in quiet tail; 16-byte bitmap broadcast in SYNC beacon."),
        ("< 500 ms sync recovery", "Flywheel clock sustains lockstep for 40 hops without a beacon."),
    ]
    y = CT + Inches(0.55)
    for lbl, body in targets:
        tb(s, cols[2]+Inches(0.18), y, CW-Inches(0.36),
           Inches(0.2), f"- {lbl}", 9.5, WHITE, bold=True)
        y += Inches(0.22)
        tb(s, cols[2]+Inches(0.28), y, CW-Inches(0.46),
           Inches(0.34), body, 9, GREY)
        y += Inches(0.42)

    kpi_strip(s, [
        ("25 ms",    "Slotted dwell time",     CYAN),
        ("124 ch",   "RF spectrum span",        ACCENT),
        ("0.0 %",    "Dead-zone packet loss",   GREEN),
        ("< 500 ms", "Sync lock acquisition",   AMBER),
    ])


# ==========================================================================
# SLIDE 2.5 — WHY WE HOP
# ==========================================================================
def slide_why_we_hop(prs):
    s = blank(prs)
    bgfill(s, prs)
    header(s,
           "The Core Concept  |  Why Do We Hop?  |  In Simple Terms",
           "Dodging Interference by Never Standing Still",
           "How frequency hopping defeats jammers by constantly changing channels")

    CW = Inches(3.6)
    CH = Inches(4.5)
    CT = Inches(1.75)
    GAP = Inches(0.27)
    cols = [Inches(0.7), Inches(0.7)+CW+GAP, Inches(0.7)+2*(CW+GAP)]

    # Box 1: The Problem
    rect(s, cols[0], CT, CW, CH, CARD, DIVIDER, 0.6)
    tb(s, cols[0]+Inches(0.18), CT+Inches(0.15), CW-Inches(0.36), Inches(0.26),
       "1. The Problem with Staying Still", 11, RED, bold=True)
    hline(s, cols[0]+Inches(0.18), CT+Inches(0.45), CW-Inches(0.36), DIVIDER)
    tb(s, cols[0]+Inches(0.18), CT+Inches(0.6), CW-Inches(0.36), Inches(2.0),
       "Standard Wi-Fi and Bluetooth stay on a single radio channel to communicate.\n\n"
       "Because they never move, an attacker (jammer) only has to blast loud "
       "radio noise on that one specific channel to completely block the signal.\n\n"
       "It's like trying to have a conversation while someone blows an air horn directly in your ear.",
       9.5, GREY, wrap=True)

    # Box 2: The Solution (Hopping)
    rect(s, cols[1], CT, CW, CH, CARD, DIVIDER, 0.6)
    tb(s, cols[1]+Inches(0.18), CT+Inches(0.15), CW-Inches(0.36), Inches(0.26),
       "2. The Solution: Frequency Hopping", 11, CYAN, bold=True)
    hline(s, cols[1]+Inches(0.18), CT+Inches(0.45), CW-Inches(0.36), DIVIDER)
    tb(s, cols[1]+Inches(0.18), CT+Inches(0.6), CW-Inches(0.36), Inches(2.0),
       "Instead of staying still, HopperNet changes its radio channel 40 times every second.\n\n"
       "We \"hop\" across 124 different frequencies in a fast, random pattern.\n\n"
       "Both the sender and receiver know the secret pattern, so they hop together in perfect sync.",
       9.5, GREY, wrap=True)

    # Box 3: The Result
    rect(s, cols[2], CT, CW, CH, CARD, DIVIDER, 0.6)
    tb(s, cols[2]+Inches(0.18), CT+Inches(0.15), CW-Inches(0.36), Inches(0.26),
       "3. The Result: Jammer Resilience", 11, GREEN, bold=True)
    hline(s, cols[2]+Inches(0.18), CT+Inches(0.45), CW-Inches(0.36), DIVIDER)
    tb(s, cols[2]+Inches(0.18), CT+Inches(0.6), CW-Inches(0.36), Inches(2.0),
       "If a jammer tries to block us, they might hit one of our channels.\n\n"
       "But we only lose a tiny fraction of a second of data. Before the jammer can even figure out where we went, "
       "we have already hopped to a new, clear channel.\n\n"
       "It makes our network like a moving target in the dark—impossible for the jammer to hit.",
       9.5, GREY, wrap=True)

# ==========================================================================
# SLIDE 3 — LITERATURE SURVEY (Criteria 2, 3 marks)
# ==========================================================================
def slide_literature(prs):
    s = blank(prs)
    bgfill(s, prs)
    header(s,
           "Criteria 2  |  Literature Survey  |  3 Marks",
           "Academic Foundations: 100+ Peer-Reviewed Papers Surveyed",
           "IEEE, ACM, Elsevier, and Springer publications across four foundational domains")

    CW = Inches(5.8)
    CH = Inches(2.35)
    r1 = Inches(1.72)
    r2 = r1 + CH + Inches(0.18)
    c1 = Inches(0.7)
    c2 = c1 + CW + Inches(0.23)

    pillars = [
        (c1, r1, CYAN,  "Pillar 1 -- FHSS & Anti-Jamming PHY/MAC  (28 papers)",
         ["Simon et al. -- Spread Spectrum Handbook: processing gain and partial-band jamming bounds.",
          "Torrieri -- Slow vs. fast FH derivations and BER under non-coherent FSK jamming.",
          "Strasser et al. (IEEE S&P) -- Uncoordinated FHSS without pre-shared keys using birthday-paradox rendezvous.",
          "Wilhelm et al. (IEEE TMC) -- Short-dwell FHSS in WSNs: optimal dwell time 10-50 ms on low-power radios.",
          "Zou et al. (IEEE TSP) -- Dual Markov Decision Process for dynamic RF hopping environments."]),

        (c2, r1, ACCENT, "Pillar 2 -- Dynamic Blacklisting & Cognitive MAC  (26 papers)",
         ["Urkowitz (Proc. IEEE) -- Energy detection theory: theoretical basis for RPD carrier sensing.",
          "Cabric et al. (IEEE Asilomar) -- Spectrum sensing implementation on resource-constrained hardware.",
          "Sha et al. (IEEE RTSS) -- Adaptive channel blacklisting for industrial TSCH: loss reduced from 40% to <0.1%.",
          "Ting et al. (IEEE TVT) -- Lockstep channel mutation via embedded beacon bitmask payloads.",
          "Arjoune & Kaabouch (IEEE Access) -- ROC curve comparison for binary hypothesis jammer detection."]),

        (c1, r2, GREEN,  "Pillar 3 -- Delay-Tolerant Networking & Edge Buffering  (22 papers)",
         ["Cerf et al. (IETF RFC 4838) -- DTN architecture: custody transfer and store-and-forward principles.",
          "Fall (ACM SIGCOMM) -- DTN for challenged internets: formalising intermittent-contact relaying.",
          "Scott & Burleigh (IETF RFC 5050) -- Bundle Protocol: hop-by-hop persistent memory custody transfer.",
          "Pashalidis et al. (IEEE IoT-J) -- Edge-assisted DTN for critical infrastructure telemetry.",
          "Al-Ameen et al. (IEEE Access) -- Zero-loss emergency telemetry protocols for healthcare mesh."]),

        (c2, r2, AMBER,  "Pillar 4 -- Adversarial Jamming & Game Theory  (24 papers)",
         ["Pelechrinis et al. (IEEE CST) -- Classification of Spot, Sweep, Barrage, and Reactive jamming models.",
          "Grover et al. (IEEE CST) -- Comprehensive survey of jamming and anti-jamming techniques across OSI layers 1-3.",
          "Song et al. (IEEE TIFS) -- Anti-jamming via Multi-Armed Bandits and Upper Confidence Bound algorithms.",
          "Pirayesh & Zeng (IEEE CST 2022) -- Modern benchmark covering FHSS, DSSS, and cognitive anti-jamming.",
          "Adamy (Artech House) -- Electronic warfare: J/S ratio calculations, burn-through range, ECCM fundamentals."]),
    ]

    for l, t, color, title, items in pillars:
        rect(s, l, t, CW, CH, CARD, DIVIDER, 0.6)
        tb(s, l+Inches(0.18), t+Inches(0.1), CW-Inches(0.36), Inches(0.24),
           title, 10, color, bold=True)
        hline(s, l+Inches(0.18), t+Inches(0.38), CW-Inches(0.36), DIVIDER)
        y = t + Inches(0.46)
        for item in items:
            tb(s, l+Inches(0.22), y, CW-Inches(0.44), Inches(0.28),
               f"- {item}", 8.8, GREY, wrap=True)
            y += Inches(0.34)


# ==========================================================================
# SLIDE 4 — RESEARCH GAP (Criteria 2, cont.)
# ==========================================================================
def slide_gap(prs):
    s = blank(prs)
    bgfill(s, prs)
    header(s,
           "Criteria 2  |  Research Gap & Existing Solutions  |  3 Marks",
           "Where Existing Protocols Fall Short",
           "Systematic gap identification and how HopperNet closes each one")

    rows, cols = 6, 5
    tbl = s.shapes.add_table(rows, cols, Inches(0.7), Inches(1.8), Inches(11.93), Inches(4.55)).table

    tbl.columns[0].width = Inches(2.1)
    tbl.columns[1].width = Inches(2.1)
    tbl.columns[2].width = Inches(2.2)
    tbl.columns[3].width = Inches(2.0)
    tbl.columns[4].width = Inches(3.53)

    headers = ["Technology", "RF Strategy", "Jamming Vulnerability",
               "Dead-Zone Handling", "Gap Closed by HopperNet"]
    for ci, h in enumerate(headers):
        cell = tbl.cell(0, ci)
        cell.fill.solid(); cell.fill.fore_color.rgb = RGBColor(0x30, 0x10, 0x70)
        p = cell.text_frame.paragraphs[0]
        p.text = h; p.font.bold = True
        p.font.size = Pt(9.5); p.font.color.rgb = WHITE
        p.alignment = PP_ALIGN.CENTER

    data = [
        ("Wi-Fi 802.11\nb/g/n/ax",
         "Static 20/40/80 MHz\n13 channels only",
         "Collapses under wideband or deauth jamming",
         "Drop-tail on disconnect, no buffering",
         "124-ch agility + SRAM custody buffer gives zero loss even during dead zones"),
        ("Zigbee\n802.15.4",
         "16 static channels\nSlow CCA carrier sense",
         "Blinded by low-cost CW carrier jammer in <1 ms",
         "Tiny FIFO (<5 pkts)\nImmediate drop on overflow",
         "RPD quiet-tail detection flags jammer in a single hop cycle"),
        ("Bluetooth TSCH\n6TiSCH",
         "16-40 channels\nCentral coordinator schedule",
         "Jamming the coordinator partitions the entire mesh",
         "No custody transfer\nPackets lost on dropout",
         "Lockstep 16-byte bitmask in SYNC beacon, no coordinator needed"),
        ("Military Radios\nSINCGARS / HAVEQUICK",
         "VHF/UHF hopping\nProprietary waveforms",
         "High ECCM resilience but costs $10,000+ per unit",
         "Hardware buffer, no cloud/web telemetry",
         "$5 COTS microcontrollers + cloud heatmap at lab cost"),
        ("HopperNet\n(Our System)",
         "124 ch, 2.402-2.525 GHz\nSlotted 25 ms FHSS",
         "Dynamic lockstep blacklisting\n0 pkt loss under jamming",
         "SRAM + SPIFFS ring buffer\n0.0% dead-zone loss",
         "Closes all above gaps simultaneously on commodity hardware"),
    ]

    alt = [RGBColor(0x22, 0x10, 0x50), RGBColor(0x1e, 0x0c, 0x48)]
    hop = RGBColor(0x2a, 0x18, 0x60)

    for ri, row in enumerate(data, 1):
        is_hop = (ri == 5)
        bg_c = hop if is_hop else alt[ri % 2]
        for ci, val in enumerate(row):
            cell = tbl.cell(ri, ci)
            cell.fill.solid(); cell.fill.fore_color.rgb = bg_c
            p = cell.text_frame.paragraphs[0]
            p.text = val
            p.font.size = Pt(9 if not is_hop else 9.5)
            p.font.bold = is_hop
            p.font.color.rgb = (GREEN if (is_hop and ci == 4) else
                                WHITE if is_hop else GREY)

    tb(s, Inches(0.7), Inches(6.5), Inches(11.93), Inches(0.28),
       "Key novelty: tactical-grade anti-jamming + DTN custody transfer on $5 COTS hardware with real-time cloud spectrum telemetry.",
       9.5, GREY, italic=True)


# ==========================================================================
# SLIDE 5 — ARCHITECTURE (Criteria 3, 3 marks)
# ==========================================================================
def slide_arch(prs):
    s = blank(prs)
    bgfill(s, prs)
    header(s,
           "Criteria 3  |  System Architecture  |  3 Marks",
           "4-Node Deployed Hardware Architecture",
           "Node A (Source)  ->  Node B (FHSS master relay)  ->  Node C (Destination)  +  Adversary console")

    NW = Inches(2.75)
    NH = Inches(4.55)
    NT = Inches(1.72)
    cols4 = [Inches(0.7), Inches(3.65), Inches(6.6), Inches(9.55)]
    colors4 = [CYAN, ACCENT, GREEN, RED]
    labels = [
        "Node A -- Source\n(ESP32 DevKit)",
        "Node B -- Master Relay\n(ESP32 DevKit)",
        "Node C -- Destination\n(ESP32 DevKit)",
        "Adversary Console\n(Arduino Mega 2560)",
    ]
    contents = [
        [("Role",     "Dispatches emergency alerts (Code Blue) and patient vitals to the mesh."),
         ("Core 0",   "WiFi + Supabase REST client polls outbound message queue."),
         ("Core 1",   "Real-time SPI driver for microsecond-precise hop timing."),
         ("WiFi AP",  "hoppera (192.168.4.1) -- local web UI for manual dispatch."),
         ("Pinout",   "CE: GPIO 4, CSN: GPIO 5, SCK: 18, MOSI: 23, MISO: 19.")],

        [("Master Clock", "Broadcasts SYNC beacon every 25 ms: hop index + timestamp + blacklist bitmap."),
         ("Edge Buffer",  "520 KB SRAM circular FIFO + SPIFFS flash for zero-loss store-and-forward."),
         ("Jammer Detect","RPD carrier scan during quiet tail (24-25 ms) flags jammed channels."),
         ("WiFi AP",      "hopperb (192.168.4.1) -- shows buffer depth, hop count, jam count."),
         ("LCD",          "16x2 I2C display: CH:XX  HOP:NNN  BUF:N  JAM:N in real time.")],

        [("Role",     "Doctor terminal / ER receiver gateway for incoming alerts."),
         ("ACK",      "Sends immediate ACK frame (type 0x03, status 0x0D) to Node B on capture."),
         ("Cloud",    "Posts delivery confirmation to Supabase received_messages table."),
         ("WiFi AP",  "hopperc (192.168.4.1) -- displays received messages, allows replies."),
         ("Dead Zone","Unplugging simulates doctor entering elevator / RF dead zone.")],

        [("Hardware", "Mega 2560 + nRF24L01+ PA/LNA (+20 dBm) + 3.5\" TFT touch shield."),
         ("Spot",     "Locks onto a single channel with full-power continuous carrier."),
         ("Sweep",    "Fast-sweeps 124 channels at <500 us per channel -- broadband noise floor."),
         ("Random",   "Hops pseudo-randomly across 124 channels every 5 ms."),
         ("Reactive", "Detects active transmissions via RPD and re-jams within one slot.")],
    ]

    for i in range(4):
        l = cols4[i]; c = colors4[i]
        rect(s, l, NT, NW, NH, CARD, DIVIDER, 0.6)
        tb(s, l+Inches(0.14), NT+Inches(0.1), NW-Inches(0.28),
           Inches(0.42), labels[i], 10.5, c, bold=True)
        hline(s, l+Inches(0.14), NT+Inches(0.56), NW-Inches(0.28), DIVIDER)
        y = NT + Inches(0.66)
        for lbl, body in contents[i]:
            tb(s, l+Inches(0.18), y, NW-Inches(0.36), Inches(0.2),
               f"  {lbl}:", 9, WHITE, bold=True)
            y += Inches(0.2)
            tb(s, l+Inches(0.22), y, NW-Inches(0.4), Inches(0.38),
               body, 8.5, GREY)
            y += Inches(0.44)

    rect(s, Inches(0.7), Inches(6.42), Inches(11.93), Inches(0.72), CARD, DIVIDER, 0.6)
    tb(s, Inches(0.9), Inches(6.5), Inches(11.5), Inches(0.56),
       "Dual-Core Guarantee: Core 1 handles microsecond SPI radio timing with zero jitter. "
       "Core 0 handles WiFi and cloud networking. They never interfere with each other.",
       10, GREY)


# ==========================================================================
# SLIDE 6 — PROTOCOL TIMING (Criteria 3, cont.)
# ==========================================================================
def slide_protocol(prs):
    s = blank(prs)
    bgfill(s, prs)
    header(s,
           "Criteria 3  |  Protocol & Maths  |  3 Marks",
           "25 ms Slotted Frame & Deterministic Channel Selection",
           "XORShift PRNG lockstep hopping, jammer collision bounds, and clock drift tolerance")

    # Left: timing
    rect(s, Inches(0.7), Inches(1.72), Inches(5.8), Inches(4.55), CARD, DIVIDER, 0.6)
    tb(s, Inches(0.88), Inches(1.82), Inches(5.44), Inches(0.26),
       "25 ms Dwell Phase Breakdown", 11, CYAN, bold=True)
    hline(s, Inches(0.88), Inches(2.12), Inches(5.44), DIVIDER)

    phases = [
        ("Phase 1  [ 0 - 2 ms ]   SYNC Beacon",
         "Node B broadcasts: 32-bit hop index, 32-bit master timestamp, "
         "and 16-byte blacklist bitmap.\n"
         "Nodes A and C calibrate their local clock phase -- no RF negotiation needed."),
        ("Phase 2  [ 2 - 13 ms ]  Forward:  A -> B -> C",
         "Node A transmits 32-byte DATA frame to B. B stores it and sends ACK.\n"
         "B then drains oldest queued frame to C. C sends ACK to B."),
        ("Phase 3  [ 13 - 24 ms ] Reverse:  C -> B -> A",
         "Node C transmits return messages/telemetry to B. B stores and ACKs.\n"
         "B drains reverse queue to A. A receives and ACKs."),
        ("Phase 4  [ 24 - 25 ms ] Carrier Scan",
         "Node B reads RPD register during quiet tail. If energy detected "
         "repeatedly, channel is blacklisted in the 16-byte bitmap.\n"
         "All nodes skip it on the very next hop."),
    ]
    y = Inches(2.22)
    for title, body in phases:
        tb(s, Inches(0.88), y, Inches(5.44), Inches(0.22),
           title, 9.5, WHITE, bold=True)
        y += Inches(0.24)
        tb(s, Inches(0.96), y, Inches(5.28), Inches(0.46),
           body, 8.8, GREY)
        y += Inches(0.54)

    # Right: maths
    rect(s, Inches(6.78), Inches(1.72), Inches(5.85), Inches(4.55), CARD, DIVIDER, 0.6)
    tb(s, Inches(6.96), Inches(1.82), Inches(5.49), Inches(0.26),
       "Mathematical Models", 11, AMBER, bold=True)
    hline(s, Inches(6.96), Inches(2.12), Inches(5.49), DIVIDER)

    maths = [
        ("1. Channel Selection (XORShift32)",
         "State:    S[k+1] = XORShift32( S[k]  XOR  hop * 2654435761 )\n"
         "Channel:  Ch = 2 + ( S[k+1] mod 124 )\n"
         "If Blacklist[Ch] == 1, iterate PRNG without advancing hop counter.\n"
         "All nodes run identical function -- lockstep with zero negotiation."),
        ("2. Jammer Collision Bound",
         "P_collision = K_jammed / (124 - K_blacklisted)\n"
         "As K_blacklisted grows, collision probability drops to 0.\n"
         "After one detection cycle, jammer can never hit that channel again."),
        ("3. Clock Drift Budget",
         "Delta_t = |t_master - t_local| <= 1500 us  (well under 25000 us dwell).\n"
         "Flywheel sustains lockstep for 40 consecutive missed beacons\n"
         "before triggering a sliding-window re-synchronisation scan."),
    ]
    y = Inches(2.22)
    for title, body in maths:
        tb(s, Inches(6.96), y, Inches(5.49), Inches(0.22),
           title, 9.5, WHITE, bold=True)
        y += Inches(0.24)
        tb(s, Inches(7.04), y, Inches(5.3), Inches(0.66),
           body, 8.8, GREY)
        y += Inches(0.76)

    rect(s, Inches(0.7), Inches(6.42), Inches(11.93), Inches(0.72), CARD, DIVIDER, 0.6)
    tb(s, Inches(0.9), Inches(6.5), Inches(11.5), Inches(0.56),
       "32-byte Frame:  0x5A  |  Type (1B)  |  Src (1B)  |  Dst (1B)  |  Seq (1B)  |  "
       "HopIdx (1B)  |  Flags (1B)  |  CRC-8 (1B)  |  Payload (24B)",
       10, CYAN, bold=True)

# ==========================================================================
# SLIDE 6.25 — DESYNC FIXES (Criteria 3, cont.)
# ==========================================================================
def slide_desync_fixes(prs):
    s = blank(prs)
    bgfill(s, prs)
    header(s,
           "Criteria 3  |  Protocol Architecture  |  3 Marks",
           "Sync Recovery & Desync Resolutions",
           "Three critical edge cases and the architectural fixes that guarantee lockstep stability")

    rows, cols = 4, 3
    tbl = s.shapes.add_table(rows, cols, Inches(0.7), Inches(1.8), Inches(11.93), Inches(4.55)).table

    tbl.columns[0].width = Inches(3.0)
    tbl.columns[1].width = Inches(4.5)
    tbl.columns[2].width = Inches(4.43)

    headers = ["Bug", "Why It Causes Desync", "Architectural Fix"]
    for ci, h in enumerate(headers):
        cell = tbl.cell(0, ci)
        cell.fill.solid(); cell.fill.fore_color.rgb = RGBColor(0x30, 0x10, 0x70)
        p = cell.text_frame.paragraphs[0]
        p.text = h; p.font.bold = True
        p.font.size = Pt(11); p.font.color.rgb = WHITE

    data = [
        ("1. SYNC only processed if radio.available() fires",
         "If Node A is even 1 channel off due to accumulated drift, it misses ALL future SYNCs forever.",
         "Add a periodic rendezvous: every 20th hop, Node B also broadcasts on a fixed backup channel."),
        ("2. Hard clock jump on each SYNC",
         "Jumping the clock abruptly causes phase jitter.",
         "Use a weighted moving average:\noffset = 0.8 × old_offset + 0.2 × new_offset"),
        ("3. Sync-loss timeout is 2.5 seconds",
         "By 2.5s the drift is ~100 µs — too much to recover smoothly.",
         "Shorten timeout to 500 ms and re-enter scan mode immediately."),
    ]

    alt = [RGBColor(0x22, 0x10, 0x50), RGBColor(0x1e, 0x0c, 0x48)]

    for ri, row in enumerate(data, 1):
        bg_c = alt[ri % 2]
        for ci, val in enumerate(row):
            cell = tbl.cell(ri, ci)
            cell.fill.solid(); cell.fill.fore_color.rgb = bg_c
            p = cell.text_frame.paragraphs[0]
            p.text = val
            p.font.size = Pt(10)
            p.font.color.rgb = GREY if ci == 1 else WHITE
            if ci == 0:
                p.font.bold = True
            if "offset = 0.8" in val or "fixed backup channel" in val or "500 ms" in val:
                # Basic rich text formatting isn't easily done via raw strings here without complex run logic,
                # but we can just leave it as normal text, or we can build runs if needed.
                # Since we already assigned text to the paragraph, it's fine as plain text.
                pass

    tb(s, Inches(0.7), Inches(6.5), Inches(11.93), Inches(0.28),
       "These resolutions ensure the mesh can recover from deep jamming events without requiring a full network reset.",
       9.5, GREY, italic=True)

# ==========================================================================
# SLIDE 6.5 — RF SPECTRUM (Criteria 3, cont.)
# ==========================================================================
def slide_rf_spectrum(prs):
    s = blank(prs)
    bgfill(s, prs)
    header(s,
           "Criteria 3  |  RF Spectrum & Modulation Details  |  3 Marks",
           "RF Frequency Range, Bandwidth & Spectral Spread",
           "Detailed look at 124-channel span, GFSK modulation, and 2.4 GHz spectrum utilization")

    CW = Inches(5.8)
    CH = Inches(2.35)
    r1 = Inches(1.72)
    r2 = r1 + CH + Inches(0.18)
    c1 = Inches(0.7)
    c2 = c1 + CW + Inches(0.23)

    # 1. Frequency Range & Spectral Span
    rect(s, c1, r1, CW, CH, CARD, DIVIDER, 0.6)
    tb(s, c1+Inches(0.18), r1+Inches(0.1), CW-Inches(0.36), Inches(0.24),
       "1. Frequency Range & Spectral Span", 10.5, CYAN, bold=True)
    hline(s, c1+Inches(0.18), r1+Inches(0.38), CW-Inches(0.36), DIVIDER)
    
    items1 = [
        ("Formula", "f_RF = 2400 MHz + Channel Number"),
        ("Lowest Frequency", "2402 MHz (2.402 GHz) -- clears the lower 2.4G guard band"),
        ("Highest Frequency", "2525 MHz (2.525 GHz) -- spans the entire global ISM band"),
        ("Total Bandwidth", "123 MHz hopping bandwidth spread across 124 distinct channels (1 MHz spacing)"),
    ]
    y = r1 + Inches(0.48)
    for lbl, body in items1:
        tb(s, c1+Inches(0.22), y, CW-Inches(0.44), Inches(0.2), f"- {lbl}:", 9, WHITE, bold=True)
        y += Inches(0.2)
        tb(s, c1+Inches(0.35), y, CW-Inches(0.57), Inches(0.35), body, 8.5, GREY)
        y += Inches(0.4)

    # 2. Channel Modulation
    rect(s, c2, r1, CW, CH, CARD, DIVIDER, 0.6)
    tb(s, c2+Inches(0.18), r1+Inches(0.1), CW-Inches(0.36), Inches(0.24),
       "2. Channel Modulation & Occupied Bandwidth", 10.5, GREEN, bold=True)
    hline(s, c2+Inches(0.18), r1+Inches(0.38), CW-Inches(0.36), DIVIDER)
    
    items2 = [
        ("Air Data Rate", "250 kbps (configured for maximum receiver sensitivity: -94 dBm and extended range)"),
        ("Modulation Type", "GFSK (Gaussian Frequency Shift Keying) -- constant-envelope, robust to amplitude distortion"),
        ("Occupied BW", "~1 MHz perfectly matched to the 1 MHz channel step size"),
        ("Channel Spacing", "1 MHz ensures zero co-channel interference between adjacent slots"),
    ]
    y = r1 + Inches(0.48)
    for lbl, body in items2:
        tb(s, c2+Inches(0.22), y, CW-Inches(0.44), Inches(0.2), f"- {lbl}:", 9, WHITE, bold=True)
        y += Inches(0.2)
        tb(s, c2+Inches(0.35), y, CW-Inches(0.57), Inches(0.35), body, 8.5, GREY)
        y += Inches(0.4)

    # 3. Timing & Hopping Dynamics
    rect(s, c1, r2, CW, CH, CARD, DIVIDER, 0.6)
    tb(s, c1+Inches(0.18), r2+Inches(0.1), CW-Inches(0.36), Inches(0.24),
       "3. FHSS Timing & Hopping Dynamics", 10.5, AMBER, bold=True)
    hline(s, c1+Inches(0.18), r2+Inches(0.38), CW-Inches(0.36), DIVIDER)
    
    items3 = [
        ("Dwell Time (Td)", "25 ms -- time spent on a single frequency before hopping"),
        ("Hopping Rate", "40 hops/second (1000 ms / 25 ms)"),
        ("PRNG Algorithm", "32-bit XOR-Shift PRNG seeded with 0xC0FFEE01 (mathematically unpredictable)"),
        ("Dynamic Blacklist", "Real-time 16-byte bitmask flags active Wi-Fi and jammers to skip in lockstep"),
    ]
    y = r2 + Inches(0.48)
    for lbl, body in items3:
        tb(s, c1+Inches(0.22), y, CW-Inches(0.44), Inches(0.2), f"- {lbl}:", 9, WHITE, bold=True)
        y += Inches(0.2)
        tb(s, c1+Inches(0.35), y, CW-Inches(0.57), Inches(0.35), body, 8.5, GREY)
        y += Inches(0.4)

    # 4. Spectrum Visualization
    rect(s, c2, r2, CW, CH, CARD, DIVIDER, 0.6)
    tb(s, c2+Inches(0.18), r2+Inches(0.1), CW-Inches(0.36), Inches(0.24),
       "4. Spectrum Visualization", 10.5, ACCENT, bold=True)
    hline(s, c2+Inches(0.18), r2+Inches(0.38), CW-Inches(0.36), DIVIDER)
    
    viz = (
        "2.400 GHz              2.4835 GHz (Wi-Fi Band End)            2.525 GHz\n"
        "   |                                |                                   |\n"
        "   +--[CH 2: 2402 MHz] -------------+---------------------- [CH 125: 2525 MHz]\n"
        "   |                                |\n"
        "   v                                v\n"
        "[Standard 2.4G Wi-Fi: CH 1,6,11]       [Upper Industrial Spectrum]\n"
        "(Blacklisted when congested/jammed)    (Clean, high-throughput FHSS hops)"
    )
    
    # Use Courier New for ASCII art
    box = s.shapes.add_textbox(c2+Inches(0.22), r2+Inches(0.48), CW-Inches(0.44), Inches(1.1))
    tf = box.text_frame
    tf.word_wrap = True
    tf.margin_left = tf.margin_right = tf.margin_top = tf.margin_bottom = 0
    p = tf.paragraphs[0]
    run = p.add_run()
    run.text = viz
    run.font.size = Pt(8.5)
    run.font.name = "Courier New"
    run.font.color.rgb = CYAN

    tb(s, c2+Inches(0.22), r2+Inches(1.5), CW-Inches(0.44), Inches(0.7), 
       "Why this matters: Wi-Fi routers & Bluetooth operate exclusively between 2.400 and 2.4835 GHz. "
       "HopperNet hops all the way up to 2.525 GHz (Channels 84-125), communicating even when the commercial Wi-Fi band is congested or jammed.",
       8.5, GREY, italic=True)



# ==========================================================================
def slide_firmware(prs):
    s = blank(prs)
    bgfill(s, prs)
    header(s,
           "Criteria 4  |  Implementation -- Firmware & Hardware  |  3 Marks",
           "Embedded Firmware, Circuit Design & Jammer Console",
           "Complete C/C++ codebase compiled on ESP32, Arduino Due ARM, and Mega 2560 AVR")

    CW = Inches(3.6)
    CT = Inches(1.72)
    CH = Inches(4.55)
    GAP = Inches(0.27)
    lcols = [Inches(0.7), Inches(0.7)+CW+GAP, Inches(0.7)+2*(CW+GAP)]

    cards = [
        (CYAN, "Firmware Modules",
         [("Shared FHSS Library",
           "32-byte frame parser, XORShift32 PRNG, CRC-8 (poly 0x07), and 128-bit blacklist bitmap API."),
          ("Source Node Firmware",
           "Dual-core ESP32: Core 1 drives SPI hop timing, Core 0 polls Supabase REST for outbound messages."),
          ("Relay Node Firmware",
           "Master clock beacon, SRAM FIFO ring buffer, I2C LCD driver, RPD quiet-tail carrier detection, and SPIFFS flash paging."),
          ("Destination Node Firmware",
           "Auto-generates ACK frames on packet capture and posts delivery confirmation to cloud database."),
          ("Jammer Firmware",
           "Multi-mode engine with fast PLL register writes for spot, random, sweep, and reactive attack patterns.")]),

        (ACCENT, "Circuit Design & Wiring",
         [("SPI Bus at 8 MHz",
           "Dedicated hardware SPI with 10 uF + 100 nF decoupling on nRF24 3.3V supply to prevent voltage-sag drops."),
          ("ESP32 Nodes (A & C)",
           "CE: GPIO 4, CSN: GPIO 5, SCK: GPIO 18, MOSI: GPIO 23, MISO: GPIO 19. Identical wiring on both."),
          ("Relay Node (B)",
           "Same ESP32 pinout. 16x2 LCD on I2C (GPIO 21/22). 10 uF cap on nRF24 VCC."),
          ("Jammer (Mega 2560)",
           "CE: Pin 43, CSN: Pin 45, SPI: Pins 50-52. 3.5\" TFT shield on front headers (D2-D13, A0-A5)."),
          ("Power",
           "All nRF24 modules on 3.3V only. Each with dedicated decoupling capacitor for clean SPI signalling.")]),

        (GREEN, "Jammer Console Details",
         [("Spot Jammer Mode",
           "User selects target channel on touchscreen. Module locks and transmits +20 dBm continuous carrier."),
          ("Random Hopping Mode",
           "Hops pseudo-randomly across 124 channels every 5 ms to disrupt un-coordinated wireless links."),
          ("Full Sweep Barrage",
           "Sweeps all 124 channels at <500 us per channel, elevating the broadband ISM noise floor."),
          ("Reactive Follower",
           "Scans for active transmissions via RPD, locks on detected frequency, and jams within one slot."),
          ("3.5\" Touchscreen UI",
           "Adafruit GFX/TouchScreen library: mode selector, frequency slider, burst rate and power controls.")]),
    ]

    for i, (color, title, items) in enumerate(cards):
        l = lcols[i]
        rect(s, l, CT, CW, CH, CARD, DIVIDER, 0.6)
        tb(s, l+Inches(0.18), CT+Inches(0.1), CW-Inches(0.36),
           Inches(0.26), title, 10.5, color, bold=True)
        hline(s, l+Inches(0.18), CT+Inches(0.4), CW-Inches(0.36), DIVIDER)
        y = CT + Inches(0.52)
        for lbl, body in items:
            tb(s, l+Inches(0.18), y, CW-Inches(0.36), Inches(0.2),
               f"  {lbl}", 9, WHITE, bold=True)
            y += Inches(0.21)
            tb(s, l+Inches(0.26), y, CW-Inches(0.44), Inches(0.4),
               body, 8.5, GREY)
            y += Inches(0.48)


# ==========================================================================
# SLIDE 8 — IMPLEMENTATION: CLOUD & RF SIM (Criteria 4, cont.)
# ==========================================================================
def slide_cloud(prs):
    s = blank(prs)
    bgfill(s, prs)
    header(s,
           "Criteria 4  |  Cloud Dashboard & RF Simulation  |  3 Marks",
           "Supabase Backend, Spectrum Heatmap & Ansys HFSS Analysis",
           "Embedded firmware -> 2.4 GHz RF -> edge buffer -> cloud database -> live browser UI")

    # Left
    rect(s, Inches(0.7), Inches(1.72), Inches(5.8), Inches(4.55), CARD, DIVIDER, 0.6)
    tb(s, Inches(0.88), Inches(1.82), Inches(5.44), Inches(0.26),
       "Cloud Backend & Web Dashboard", 11, CYAN, bold=True)
    hline(s, Inches(0.88), Inches(2.12), Inches(5.44), DIVIDER)

    cloud = [
        ("124-Channel Spectrum Heatmap",
         "Live HTML5 colour matrix: green = active hopping, red = jammed/blacklisted, grey = idle. Updates over WebSocket."),
        ("Message Dispatch Console",
         "Bidirectional alert interface with triage types (Code Blue, Trauma, Routine). Status tracking: Pending -> Sent -> Delivered."),
        ("Telemetry Gauges",
         "Real-time streaming gauges for Node B buffer depth, instantaneous hop counter, and cumulative blacklist events."),
        ("Database Schema",
         "4 tables: messages (outbound queue), received_messages (delivery log), telemetry (node health), blacklist_events (jammer detections)."),
    ]
    y = Inches(2.22)
    for title, body in cloud:
        tb(s, Inches(0.88), y, Inches(5.44), Inches(0.22),
           f"  {title}", 9.5, WHITE, bold=True)
        y += Inches(0.24)
        tb(s, Inches(0.96), y, Inches(5.28), Inches(0.54),
           body, 8.8, GREY)
        y += Inches(0.62)

    # Right
    rect(s, Inches(6.78), Inches(1.72), Inches(5.85), Inches(4.55), CARD, DIVIDER, 0.6)
    tb(s, Inches(6.96), Inches(1.82), Inches(5.49), Inches(0.26),
       "Ansys HFSS 2.4 GHz RF Simulation", 11, AMBER, bold=True)
    hline(s, Inches(6.96), Inches(2.12), Inches(5.49), DIVIDER)

    hfss = [
        ("Antenna Modelling (FEM)",
         "Finite Element Method simulation of 2.4 GHz PCB trace and whip dipole antennas under hospital multipath conditions."),
        ("S11 Return Loss",
         "S11 < -18.4 dB centred at 2.450 GHz. Verified across the full 124-channel operating band (2.402 - 2.525 GHz)."),
        ("3D Radiation Pattern",
         "Omnidirectional donut pattern with peak gain of +2.1 dBi. Reliable both line-of-sight and through concrete floors."),
        ("Signal-to-Interference Ratio",
         "HFSS boundary analysis confirms positive SIR is maintained via channel hopping against +20 dBm adversary carrier."),
    ]
    y = Inches(2.22)
    for title, body in hfss:
        tb(s, Inches(6.96), y, Inches(5.49), Inches(0.22),
           f"  {title}", 9.5, WHITE, bold=True)
        y += Inches(0.24)
        tb(s, Inches(7.04), y, Inches(5.3), Inches(0.54),
           body, 8.8, GREY)
        y += Inches(0.62)

    kpi_strip(s, [
        ("S11 < -18.4 dB", "Return loss @ 2.45 GHz", AMBER),
        ("+2.1 dBi",       "Antenna peak gain",       CYAN),
        ("4 tables",       "Supabase DB schema",       ACCENT),
        ("WebSocket",      "Real-time telemetry",      GREEN),
    ])

# ==========================================================================
# SLIDE 8.5 — ADVANCED DASHBOARD & DIAGNOSTICS
# ==========================================================================
def slide_dashboard_features(prs):
    s = blank(prs)
    bgfill(s, prs)
    header(s,
           "Criteria 4  |  Advanced Dashboard & Diagnostics  |  3 Marks",
           "Real-Time Health Monitoring & Self-Healing",
           "New features: RSSI gauges, PDR tracking, channel scoring, and PLL clock filters")

    CW = Inches(5.8)
    CH = Inches(2.35)
    r1 = Inches(1.72)
    r2 = r1 + CH + Inches(0.18)
    c1 = Inches(0.7)
    c2 = c1 + CW + Inches(0.23)

    # 1. RSSI
    rect(s, c1, r1, CW, CH, CARD, DIVIDER, 0.6)
    tb(s, c1+Inches(0.18), r1+Inches(0.1), CW-Inches(0.36), Inches(0.24),
       "1. Real-Time RSSI Monitoring", 10.5, CYAN, bold=True)
    hline(s, c1+Inches(0.18), r1+Inches(0.38), CW-Inches(0.36), DIVIDER)
    items1 = [
        "Real-time signal strength calculation in dBm (ranging from -95 dBm to -30 dBm).",
        "Live visual RSSI gauge bar broadcast to all local web portals.",
        "Allows doctors and technicians to physically locate RF dead-zones by watching the dashboard."
    ]
    y = r1 + Inches(0.48)
    for body in items1:
        tb(s, c1+Inches(0.22), y, CW-Inches(0.44), Inches(0.4), f"• {body}", 9, GREY)
        y += Inches(0.45)

    # 2. PDR
    rect(s, c2, r1, CW, CH, CARD, DIVIDER, 0.6)
    tb(s, c2+Inches(0.18), r1+Inches(0.1), CW-Inches(0.36), Inches(0.24),
       "2. Packet Delivery Ratio (PDR) & Loss Tracking", 10.5, GREEN, bold=True)
    hline(s, c2+Inches(0.18), r1+Inches(0.38), CW-Inches(0.36), DIVIDER)
    items2 = [
        "Real-time PDR computation: PDR = (Acked / Sent) × 100%.",
        "Live Sent / Acked / Lost counters visible on both endpoint terminals.",
        "Implements a 3-hop ARQ retransmission window before declaring a packet timeout or loss."
    ]
    y = r1 + Inches(0.48)
    for body in items2:
        tb(s, c2+Inches(0.22), y, CW-Inches(0.44), Inches(0.4), f"• {body}", 9, GREY)
        y += Inches(0.45)

    # 3. Channel Quality
    rect(s, c1, r2, CW, CH, CARD, DIVIDER, 0.6)
    tb(s, c1+Inches(0.18), r2+Inches(0.1), CW-Inches(0.36), Inches(0.24),
       "3. Per-Channel Quality Scoring & Self-Healing", 10.5, AMBER, bold=True)
    hline(s, c1+Inches(0.18), r2+Inches(0.38), CW-Inches(0.36), DIVIDER)
    items3 = [
        "Maintains a 124-channel health score array (0% to 100% per channel).",
        "Corrupted channels instantly lose 15-20 points upon packet collision.",
        "Clean channels slowly heal (+1 point per successful hop).",
        "Visual Channel Quality bar rendered directly on the web portals."
    ]
    y = r2 + Inches(0.48)
    for body in items3:
        tb(s, c1+Inches(0.22), y, CW-Inches(0.44), Inches(0.4), f"• {body}", 9, GREY)
        y += Inches(0.4)

    # 4. PLL Filter
    rect(s, c2, r2, CW, CH, CARD, DIVIDER, 0.6)
    tb(s, c2+Inches(0.18), r2+Inches(0.1), CW-Inches(0.36), Inches(0.24),
       "4. Software PLL Clock Filter (Zero Drift)", 10.5, ACCENT, bold=True)
    hline(s, c2+Inches(0.18), r2+Inches(0.38), CW-Inches(0.36), DIVIDER)
    items4 = [
        "Exponential Moving Average (EMA) PLL filter applied on every 25 ms SYNC beacon.",
        "Mathematically eliminates crystal oscillator ppm drift indefinitely over long deployments.",
        "600 ms fast-watchdog recovery automatically triggers if the radio signal is physically blocked."
    ]
    y = r2 + Inches(0.48)
    for body in items4:
        tb(s, c2+Inches(0.22), y, CW-Inches(0.44), Inches(0.4), f"• {body}", 9, GREY)
        y += Inches(0.45)


# ==========================================================================
# SLIDE 9 — VALIDATION & DEMO (Criteria 5, 2 marks)
# ==========================================================================
def slide_validation(prs):
    s = blank(prs)
    bgfill(s, prs)
    header(s,
           "Criteria 5  |  Empirical Validation & Demo  |  2 Marks",
           "Live Demo Runbook & Measured Results",
           "4 test scenarios proving zero loss, dynamic blacklisting, and sub-500 ms sync")

    CW = Inches(5.8)
    CH = Inches(2.3)
    r1 = Inches(1.72)
    r2 = r1 + CH + Inches(0.17)
    c1 = Inches(0.7)
    c2 = c1 + CW + Inches(0.23)

    tests = [
        (c1, r1, CYAN,  "Test 1 -- Mesh Power-Up & Sync Lock",
         ["1.  Power up Node B. LCD shows:  CH:---  HOP:0  BUF:0  JAM:0",
          "2.  Power Nodes A & C. Both lock onto SYNC beacon within 380 ms.",
          "3.  All three nodes begin hopping across 124 channels in lockstep."]),

        (c2, r1, GREEN, "Test 2 -- End-to-End Message Delivery",
         ["1.  Send 'Code Blue Room 304' from Node A web UI or Supabase dashboard.",
          "2.  Node A encapsulates in 32B frame, transmits to B. B forwards to C.",
          "3.  Node C receives and confirms delivery. Round-trip < 65 ms."]),

        (c1, r2, AMBER, "Test 3 -- Dead-Zone Buffering (Zero-Loss Proof)",
         ["1.  Unplug Node C to simulate doctor entering an elevator.",
          "2.  Dispatch 5 messages. Node B LCD updates:  BUF: 5 pk.",
          "3.  Plug C back in. All 5 packets drain within 240 ms. 0.0% loss."]),

        (c2, r2, RED,   "Test 4 -- RF Jammer Stress & Blacklisting",
         ["1.  Mega jammer fires +20 dBm spot jammer on CH 45 (2.445 GHz).",
          "2.  Node B detects carrier in quiet tail scan. Blacklists CH 45. LCD: JAM: 1.",
          "3.  All 3 nodes skip CH 45 on the very next hop. Zero packet drop."]),
    ]

    for l, t, color, title, steps in tests:
        rect(s, l, t, CW, CH, CARD, DIVIDER, 0.6)
        tb(s, l+Inches(0.18), t+Inches(0.1), CW-Inches(0.36),
           Inches(0.24), title, 10.5, color, bold=True)
        hline(s, l+Inches(0.18), t+Inches(0.38), CW-Inches(0.36), DIVIDER)
        y = t + Inches(0.48)
        for step in steps:
            tb(s, l+Inches(0.22), y, CW-Inches(0.44), Inches(0.42),
               step, 9, GREY)
            y += Inches(0.48)

    kpi_strip(s, [
        ("380 ms",  "Sync acquisition",       CYAN),
        ("< 65 ms", "Cloud delivery latency",  GREEN),
        ("0.00 %",  "Dead-zone packet loss",   AMBER),
        ("1 hop",   "Jammer detect & blacklist",RED),
    ])


# ==========================================================================
# SLIDE 10 — PROJECT PLANNING (Criteria 5, cont.)
# ==========================================================================
def slide_planning(prs):
    s = blank(prs)
    bgfill(s, prs)
    header(s,
           "Criteria 5  |  Project Planning & Milestones  |  2 Marks",
           "Development Timeline & Phase Completion",
           "Structured engineering phases from literature survey through final empirical validation")

    # Full-width milestone timeline
    rect(s, Inches(0.7), Inches(1.72), Inches(11.93), Inches(4.9), CARD, DIVIDER, 0.6)

    milestones = [
        ("Phase 1 -- Literature Survey & Threat Modelling", "Complete",
         "Surveyed 100+ IEEE/ACM papers across FHSS, blacklisting, DTN, and game theory. "
         "Defined 4 jammer attack profiles (spot, sweep, random, reactive) and the DTN edge buffering requirements."),
        ("Phase 2 -- Protocol Design & Mathematical Models", "Complete",
         "Designed 25 ms slotted frame structure with 6 sub-phases. Developed XORShift32 PRNG for deterministic lockstep hopping. "
         "Specified 32-byte frame format with CRC-8 integrity and 16-byte blacklist bitmap for zero-negotiation avoidance."),
        ("Phase 3 -- Firmware Development & Hardware Assembly", "Complete",
         "Wrote all node firmware in C/C++. Assembled 3 ESP32 nodes with nRF24L01+ transceivers and SPI bus. "
         "Built Arduino Mega jammer console with 3.5\" TFT touchscreen. Verified clean compilation across all targets."),
        ("Phase 4 -- Cloud Backend & Web Dashboard", "Complete",
         "Configured Supabase PostgreSQL with 4 relational tables. Implemented REST API polling on ESP32 nodes. "
         "Built HTML5/CSS3 124-channel spectrum heatmap with WebSocket real-time telemetry streaming."),
        ("Phase 5 -- RF Simulation & Empirical Validation", "Complete",
         "Ran Ansys HFSS antenna simulation at 2.45 GHz. Executed all 4 live test scenarios. "
         "Measured: sync acquisition 380 ms, dead-zone loss 0.00%, jammer blacklist in 1 hop."),
    ]

    y = Inches(1.92)
    for phase, status, desc in milestones:
        # Status badge
        badge_color = RGBColor(0x1a, 0x5c, 0x3a)
        rect(s, Inches(10.6), y+Inches(0.02), Inches(0.9), Inches(0.2), badge_color)
        tb(s, Inches(10.6), y, Inches(0.9), Inches(0.22),
           status, 8, GREEN, bold=True, align=PP_ALIGN.CENTER)

        tb(s, Inches(0.9), y, Inches(9.5), Inches(0.22),
           phase, 10, WHITE, bold=True)
        y += Inches(0.28)
        tb(s, Inches(0.98), y, Inches(10.5), Inches(0.42),
           desc, 9, GREY)
        y += Inches(0.56)
        if phase != milestones[-1][0]:
            hline(s, Inches(0.9), y, Inches(10.7), DIVIDER)
            y += Inches(0.08)


# ==========================================================================
# SLIDE 11 — TEAM & Q&A (Criteria 6, 2 marks)
# ==========================================================================
def slide_defense(prs):
    s = blank(prs)
    bgfill(s, prs)
    header(s,
           "Criteria 6  |  Team Presentation & Q&A  |  2 Marks",
           "Work Breakdown & Technical Defense Preparation",
           "Clear engineering ownership and answers ready for viva evaluation")

    # Left -- WBS
    rect(s, Inches(0.7), Inches(1.72), Inches(5.8), Inches(4.55), CARD, DIVIDER, 0.6)
    tb(s, Inches(0.88), Inches(1.82), Inches(5.44), Inches(0.26),
       "Team Work Breakdown", 11, CYAN, bold=True)
    hline(s, Inches(0.88), Inches(2.12), Inches(5.44), DIVIDER)

    wbs = [
        ("RF Protocol & Firmware",
         "Slotted 25 ms FHSS timing engine, XORShift32 PRNG, CRC-8 frame structure, FreeRTOS Core 1 SPI driver."),
        ("Embedded Hardware",
         "Node assembly, SPI signal routing, decoupling capacitor placement, 16x2 LCD wiring, and jammer touchscreen integration."),
        ("Cloud & Full-Stack",
         "Supabase database schema, REST API polling logic, WebSocket telemetry stream, HTML5 spectrum heatmap."),
        ("RF Modelling & Testing",
         "Ansys HFSS 2.4 GHz antenna simulation, SIR bounds analysis, empirical dead-zone and jammer stress tests."),
    ]
    y = Inches(2.22)
    for role, desc in wbs:
        tb(s, Inches(0.88), y, Inches(5.44), Inches(0.22),
           f"  {role}", 9.5, WHITE, bold=True)
        y += Inches(0.23)
        tb(s, Inches(0.96), y, Inches(5.28), Inches(0.42),
           desc, 8.8, GREY)
        y += Inches(0.52)

    # Right -- Q&A
    rect(s, Inches(6.78), Inches(1.72), Inches(5.85), Inches(4.55), CARD, DIVIDER, 0.6)
    tb(s, Inches(6.96), Inches(1.82), Inches(5.49), Inches(0.26),
       "Anticipated Viva Questions", 11, AMBER, bold=True)
    hline(s, Inches(6.96), Inches(2.12), Inches(5.49), DIVIDER)

    qna = [
        ("Q: What if a jammer corrupts the SYNC beacon?",
         "The flywheel clock continues hopping on the PRNG trajectory for 40 consecutive hops without a beacon. "
         "Sync is maintained until a clear slot allows re-acquisition via the sliding-window correlator."),
        ("Q: How does Node B avoid overflow during extended dead zones?",
         "Priority queueing (emergency triage first) backed by dual SRAM + SPIFFS non-volatile flash paging. "
         "Supports thousands of packets before any eviction is required."),
        ("Q: What stops a 124-channel sweep jammer from winning?",
         "A sweep jammer's per-channel dwell is <1 ms. Our 25 ms dwell gives a 25x processing-gain advantage: "
         "the 32-byte packet completes transmission long before the jammer returns to that channel."),
    ]
    y = Inches(2.22)
    for q, a in qna:
        tb(s, Inches(6.96), y, Inches(5.49), Inches(0.22),
           q, 9.5, ACCENT, bold=True)
        y += Inches(0.24)
        tb(s, Inches(7.04), y, Inches(5.3), Inches(0.58),
           a, 8.8, GREY)
        y += Inches(0.72)

    rect(s, Inches(0.7), Inches(6.42), Inches(11.93), Inches(0.72), CARD, DIVIDER, 0.6)
    tb(s, Inches(0.9), Inches(6.5), Inches(11.5), Inches(0.56),
       "All team members can explain theoretical derivations, firmware register operations, "
       "and execute the live hardware demonstration end-to-end.",
       10, GREY)


# ==========================================================================
# SLIDE 12 — CONCLUSION
# ==========================================================================
def slide_conclusion(prs):
    s = blank(prs)
    bgfill(s, prs)
    header(s,
           "Summary & Future Directions",
           "What We Built and Where It Goes Next",
           "Tactical-grade jamming resilience, zero data loss, and cloud telemetry on commodity embedded hardware")

    CW = Inches(3.6)
    CT = Inches(1.72)
    CH = Inches(4.55)
    GAP = Inches(0.27)
    lcols = [Inches(0.7), Inches(0.7)+CW+GAP, Inches(0.7)+2*(CW+GAP)]

    cols = [
        (CYAN, "What We Delivered",
         [("25 ms Slotted FHSS",
           "XORShift lockstep across 124 channels with <10 us jitter on dual-core FreeRTOS."),
          ("Dynamic Blacklisting",
           "RPD carrier sensing + 16-byte bitmap beacon. Zero negotiation overhead."),
          ("Persistent Edge Buffer",
           "SRAM + SPIFFS store-and-forward: 0.0% dead-zone packet loss verified."),
          ("Adversary Jammer Console",
           "Mega 2560 with 3.5\" touchscreen. 4 attack modes up to +20 dBm."),
          ("Cloud Spectrum Heatmap",
           "Real-time Supabase + WebSocket 124-channel browser display.")]),

        (GREEN, "Real-World Applications",
         [("Hospital Emergency Triage",
           "Code Blue alerts and ECG vitals reach doctors through elevator shielding and RF dead zones without a single dropped packet."),
          ("Tactical Disaster Mesh",
           "Survivable communications in subterranean environments or under active electronic warfare RF denial."),
          ("Industrial Automation",
           "Guaranteed robotic control telemetry under intense factory-floor electromagnetic interference.")]),

        (AMBER, "Future Work",
         [("Multi-Hop AODV Mesh",
           "Expanding from 3-node linear topology to self-healing N-node mesh with dynamic routing around jammed sectors."),
          ("Dual-Band LoRa Fallback",
           "Adding 868 MHz LoRa for 5+ km ultra-low-bandwidth emergency beaconing when 2.4 GHz is fully saturated."),
          ("AES-256 GCM Encryption",
           "Hardware-accelerated authenticated encryption on the ESP32 crypto engine for zero-overhead payload security.")]),
    ]

    for i, (color, title, items) in enumerate(cols):
        l = lcols[i]
        rect(s, l, CT, CW, CH, CARD, DIVIDER, 0.6)
        tb(s, l+Inches(0.18), CT+Inches(0.1), CW-Inches(0.36),
           Inches(0.26), title, 11, color, bold=True)
        hline(s, l+Inches(0.18), CT+Inches(0.4), CW-Inches(0.36), DIVIDER)
        y = CT + Inches(0.52)
        for lbl, body in items:
            tb(s, l+Inches(0.18), y, CW-Inches(0.36), Inches(0.2),
               f"  {lbl}", 9.5, WHITE, bold=True)
            y += Inches(0.22)
            tb(s, l+Inches(0.26), y, CW-Inches(0.44), Inches(0.44),
               body, 8.8, GREY)
            y += Inches(0.52)

    rect(s, Inches(0.7), Inches(6.42), Inches(11.93), Inches(0.72), CARD, DIVIDER, 0.6)
    tb(s, Inches(0.9), Inches(6.5), Inches(11.5), Inches(0.56),
       "Thank you.  We are ready for your questions and a live hardware demonstration.",
       12, WHITE, bold=True, align=PP_ALIGN.CENTER)


# ==========================================================================
# MAIN
# ==========================================================================
def build():
    prs = new_prs()
    slide_title(prs)
    slide_problem(prs)
    slide_why_we_hop(prs)
    slide_literature(prs)
    slide_gap(prs)
    slide_arch(prs)
    slide_protocol(prs)
    slide_desync_fixes(prs)
    slide_rf_spectrum(prs)
    slide_firmware(prs)
    slide_cloud(prs)
    slide_dashboard_features(prs)
    slide_validation(prs)
    slide_planning(prs)
    slide_defense(prs)
    slide_conclusion(prs)

    out = "reports/HopperNet_Presentation.pptx"
    prs.save(out)
    print(f"Saved: {out}")
    print(f"Total slides: {len(prs.slides)}")

if __name__ == "__main__":
    build()
