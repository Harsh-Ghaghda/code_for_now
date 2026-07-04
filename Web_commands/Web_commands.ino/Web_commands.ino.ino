#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>

// Set up AP credentials
const char* ssid = "MediTilt_Pro_Net";
const char* password = "clinicalcontrol"; // Must be at least 8 characters

ESP8266WebServer server(80);

// Bed State Variables
int headAngle = 0;
int footAngle = 0;
int leftTilt = 0;
int rightTilt = 0;

// The raw HTML code matching your design
const char html_page[] PROGMEM = R"=====(
<!doctype html>
<html lang="en">
  <head>
    <meta charset="UTF-8" />
    <meta name="viewport" content="width=device-width, initial-scale=1.0, maximum-scale=1.0, user-scalable=no, viewport-fit=cover" />
    <title>MediTilt Pro — Clinical Control Station</title>
    <style>
      @import url("https://fonts.googleapis.com/css2?family=Plus+Jakarta+Sans:wght@400;500;600;700;800&display=swap");
      :root {
        --bg: #0b0f17; --panel: #131924; --card: #1a2232; --border: rgba(255, 255, 255, 0.06);
        --accent: #00f2fe; --accent-glow: rgba(0, 242, 254, 0.2); --mint: #05f2c7;
        --text: #f8fafc; --text-muted: #64748b; --radius-lg: 1rem; --radius-sm: 0.5rem;
        --transition: 0.25s cubic-bezier(0.4, 0, 0.2, 1); --drag-motion: 0.1s linear; --smooth-motion: 2.5s cubic-bezier(0.1, 0.8, 0.25, 1);
      }
      * { margin: 0; padding: 0; box-sizing: border-box; font-family: "Plus Jakarta Sans", sans-serif; -webkit-font-smoothing: antialiased; }
      body {
        background-color: var(--bg);
        background-image: radial-gradient(at 0% 0%, rgba(0, 242, 254, 0.05) 0px, transparent 50%), radial-gradient(at 100% 100%, rgba(5, 242, 199, 0.03) 0px, transparent 50%);
        min-height: 100vh; display: flex; align-items: center; justify-content: center; padding: 1rem; color: var(--text);
      }
      .dashboard-container { width: 100%; max-width: 1100px; background: var(--panel); border: 1px solid var(--border); border-radius: var(--radius-lg); padding: 1.5rem; box-shadow: 0 40px 80px -20px rgba(0, 0, 0, 0.7); display: flex; flex-direction: column; gap: 1.5rem; }
      header { display: flex; justify-content: space-between; align-items: center; border-bottom: 1px solid var(--border); padding-bottom: 1rem; }
      .brand h1 { font-size: 1.5rem; font-weight: 800; letter-spacing: -0.5px; background: linear-gradient(120deg, #fff, #94a3b8); -webkit-background-clip: text; -webkit-text-fill-color: transparent; }
      .brand p { font-size: 0.7rem; font-weight: 600; text-transform: uppercase; letter-spacing: 2px; color: var(--mint); margin-top: 0.2rem; }
      .status-badge { font-size: 0.7rem; font-weight: 700; background: rgba(5, 242, 199, 0.1); color: var(--mint); padding: 0.4rem 0.8rem; border-radius: 2rem; border: 1px solid rgba(5, 242, 199, 0.2); }
      .main-grid { display: grid; grid-template-columns: 1fr; gap: 1.25rem; }
      @media (min-width: 900px) { .main-grid { grid-template-columns: 1.2fr 1fr; } }
      .section-column { display: flex; flex-direction: column; gap: 1.25rem; }
      .panel-card { background: var(--card); border: 1px solid var(--border); border-radius: var(--radius-lg); padding: 1.25rem; }
      .panel-card h2 { font-size: 0.85rem; font-weight: 700; text-transform: uppercase; letter-spacing: 1.5px; color: var(--text-muted); margin-bottom: 1rem; display: flex; align-items: center; gap: 0.5rem; }
      .visual-viewport { display: flex; flex-direction: column; gap: 1rem; background: rgba(0, 0, 0, 0.2); padding: 1rem; border-radius: var(--radius-sm); border: 1px solid rgba(255, 255, 255, 0.02); }
      .view-container { position: relative; height: 90px; width: 100%; display: flex; align-items: flex-end; justify-content: center; overflow: hidden; user-select: none; }
      @media (min-width: 900px) { .view-container { height: 120px; } }
      .base-chassis { position: absolute; bottom: 10px; width: 90%; height: 6px; background: #334155; border-radius: 4px; }
      .chassis-post { position: absolute; bottom: 16px; width: 8px; height: 25px; background: #475569; }
      .chassis-post.left { left: 8%; border-radius: 4px 4px 0 0; }
      .chassis-post.right { right: 8%; border-radius: 4px 4px 0 0; }
      .mattress-linkage { position: absolute; bottom: 22px; width: 78%; height: 20px; display: flex; align-items: flex-end; }
      .center-pelvic-node { width: 26%; height: 14px; background: #1e293b; border: 1px solid #334155; border-radius: 3px; margin: 0 2px; z-index: 2; }
      .head-node { width: 37%; height: 14px; background: linear-gradient(90deg, var(--accent), var(--mint)); border-radius: 3px; box-shadow: 0 0 15px var(--accent-glow); transition: transform var(--smooth-motion); will-change: transform; transform-origin: right bottom; cursor: grab; touch-action: none; }
      .head-node:active { cursor: grabbing; }
      .foot-node-container { width: 37%; height: 14px; position: relative; transform-origin: left bottom; transition: transform var(--smooth-motion); will-change: transform; cursor: grab; touch-action: none; }
      .foot-node-container:active { cursor: grabbing; }
      .foot-node-thigh { width: 50%; height: 100%; background: linear-gradient(90deg, var(--accent), #02decb); border-radius: 3px 0 0 3px; position: absolute; left: 0; top: 0; }
      .foot-node-flap { width: 50%; height: 100%; background: linear-gradient(90deg, #02decb, var(--mint)); border-radius: 0 3px 3px 0; position: absolute; left: 50%; top: 0; transform-origin: left top; transition: transform var(--smooth-motion); will-change: transform; box-shadow: 3px 0 10px var(--accent-glow); }
      .transverse-container { align-items: center; }
      .transverse-deck { position: relative; width: 65%; height: 16px; }
      .deck-wing-l, .deck-wing-r { position: absolute; width: 49.5%; height: 12px; background: linear-gradient(135deg, var(--accent), var(--mint)); box-shadow: 0 0 15px var(--accent-glow); transition: transform var(--smooth-motion); will-change: transform; cursor: grab; touch-action: none; }
      .deck-wing-l:active, .deck-wing-r:active { cursor: grabbing; }
      .deck-wing-l { left: 0; border-radius: 4px 0 0 4px; transform-origin: right bottom; transform: rotate(0deg); }
      .deck-wing-r { right: 0; border-radius: 0 4px 4px 0; transform-origin: left bottom; transform: rotate(0deg); }
      .dragging-active { transition: transform var(--drag-motion) !important; }
      .pivot-pin { position: absolute; bottom: -4px; left: 50%; transform: translateX(-50%); width: 10px; height: 10px; background: #fff; border-radius: 50%; box-shadow: 0 0 10px #fff; z-index: 5; }
      .telemetry-output { display: flex; justify-content: space-between; font-size: 0.75rem; font-weight: 700; color: var(--text); background: rgba(255, 255, 255, 0.03); padding: 0.5rem 0.75rem; border-radius: var(--radius-sm); border: 1px solid var(--border); }
      .telemetry-output span { color: var(--accent); }
      .control-block { display: flex; flex-direction: column; gap: 1rem; }
      .control-subgroup { background: rgba(0, 0, 0, 0.15); padding: 1rem; border-radius: var(--radius-sm); border: 1px solid var(--border); display: flex; flex-direction: column; gap: 0.75rem; }
      .control-subgroup h3 { font-size: 0.75rem; color: var(--text-muted); text-transform: uppercase; letter-spacing: 0.5px; }
      .button-row { display: flex; gap: 0.4rem; flex-wrap: wrap; }
      .btn { background: #222d3d; border: 1px solid var(--border); border-radius: var(--radius-sm); padding: 0.65rem 0.85rem; font-weight: 600; font-size: 0.8rem; color: var(--text); cursor: pointer; transition: var(--transition); display: inline-flex; align-items: center; justify-content: center; gap: 0.4rem; flex: 1 1 auto; }
      .btn:hover { background: #2a374a; border-color: rgba(255, 255, 255, 0.15); }
      .btn:active { transform: scale(0.97); }
      .btn.preset { min-width: 40px; flex: 1; }
      .btn.preset.active { background: linear-gradient(135deg, var(--accent), var(--mint)); color: var(--bg); font-weight: 700; border-color: transparent; box-shadow: 0 0 15px var(--accent-glow); }
      .header-toggle-wrap { display: flex; justify-content: space-between; align-items: center; margin-bottom: 1rem; }
      .switch-box { display: flex; align-items: center; gap: 0.75rem; font-weight: 600; font-size: 0.85rem; }
      .switch-box span.active-state { color: var(--text-muted); }
      input:checked ~ span.active-state { color: var(--accent); }
      .switch { position: relative; width: 44px; height: 22px; }
      .switch input { opacity: 0; width: 0; height: 0; }
      .slider { position: absolute; cursor: pointer; top: 0; left: 0; right: 0; bottom: 0; background: #2d394a; border-radius: 2rem; transition: var(--transition); }
      .slider:before { content: ""; position: absolute; height: 16px; width: 16px; left: 3px; bottom: 3px; background: #fff; border-radius: 50%; transition: var(--transition); }
      input:checked + .slider { background: var(--accent); }
      input:checked + .slider:before { transform: translateX(22px); background: var(--bg); }
      .blink-terminal { background: rgba(0, 0, 0, 0.3); border-radius: var(--radius-sm); border: 1px dashed rgba(0, 242, 254, 0.25); padding: 1rem; display: none; flex-direction: column; gap: 1rem; }
      .blink-terminal.active { display: flex; }
      .blink-chip { background: #19202e; border: 1px solid var(--border); border-radius: 4px; padding: 0.5rem; font-size: 0.75rem; font-weight: 600; color: var(--text-muted); text-align: center; transition: var(--transition); }
      .blink-chip.current { background: rgba(0, 242, 254, 0.08); color: var(--accent); border-color: var(--accent); font-weight: 700; }
      .selection-focus { font-size: 0.8rem; font-weight: 600; color: var(--text-muted); }
      .selection-focus span { background: var(--accent); color: var(--bg); font-weight: 700; padding: 0.2rem 0.6rem; border-radius: 3px; margin-left: 0.4rem; }
      @media (max-width: 600px) { body { padding: 0.5rem; } .dashboard-container { padding: 1rem; gap: 1rem; } header { flex-direction: column; align-items: flex-start; gap: 0.75rem; } .status-badge { align-self: flex-start; } }
    </style>
  </head>
  <body>
    <div class="dashboard-container">
      <header>
        <div class="brand"><h1>MediTilt Pro</h1><p>Decubitus Prevention System</p></div>
        <div class="status-badge">System Core Online</div>
      </header>
      <div class="main-grid">
        <div class="section-column">
          <div class="panel-card control-block">
            <h2>📐 Head & Foot (Sagittal Kinematics)</h2>
            <div class="visual-viewport">
              <div class="view-container">
                <div class="base-chassis"></div><div class="chassis-post left"></div><div class="chassis-post right"></div>
                <div class="mattress-linkage">
                  <div class="head-node" id="headSeg"></div><div class="center-pelvic-node"></div>
                  <div class="foot-node-container" id="footSegContainer"><div class="foot-node-thigh"></div><div class="foot-node-flap" id="footFlapSeg"></div></div>
                </div>
              </div>
              <div class="telemetry-output">Position Monitor <span id="sagDisplay">Head 0° · Foot 0°</span></div>
            </div>
            <div class="control-subgroup">
              <h3>Head Section Adjustment</h3>
              <div class="button-row" id="headPresetGroup">
                <button class="btn preset" data-angle="0">0°</button><button class="btn preset" data-angle="15">15°</button><button class="btn preset" data-angle="30">30°</button><button class="btn preset" data-angle="45">45°</button><button class="btn preset" data-angle="60">60°</button>
              </div>
              <div class="button-row"><button class="btn" id="headUpBtn">▲ Raise Head</button><button class="btn" id="headDownBtn">▼ Lower Head</button></div>
            </div>
            <div class="control-subgroup">
              <h3>Foot Section Adjustment</h3>
              <div class="button-row" id="footPresetGroup">
                <button class="btn preset" data-angle="0">0°</button><button class="btn preset" data-angle="15">15°</button><button class="btn preset" data-angle="30">30°</button>
              </div>
              <div class="button-row"><button class="btn" id="footUpBtn">▲ Raise Foot</button><button class="btn" id="footDownBtn">▼ Lower Foot</button></div>
            </div>
          </div>
        </div>
        <div class="section-column">
          <div class="panel-card control-block">
            <h2>⚖️ Lateral Turn & Balance</h2>
            <div class="visual-viewport">
              <div class="view-container transverse-container"><div class="transverse-deck"><div class="deck-wing-l" id="leftHalf"></div><div class="deck-wing-r" id="rightHalf"></div><div class="pivot-pin"></div></div></div>
              <div class="telemetry-output">Transverse Balance <span id="latDisplay">Left 0° · Right 0°</span></div>
            </div>
            <div class="control-subgroup">
              <h3>Lateral Tilt Rotation</h3>
              <div class="button-row"><button class="btn" id="leftUpBtn">◀ Left Up</button><button class="btn" id="leftDownBtn">Left Down ▶</button></div>
              <div class="button-row"><button class="btn" id="rightUpBtn">▶ Right Up</button><button class="btn" id="rightDownBtn">◀ Right Down</button></div>
            </div>
          </div>
          <div class="panel-card">
            <div class="header-toggle-wrap">
              <h2>👁️ Eye-Blink Control</h2>
              <div class="switch-box"><span>Off</span><label class="switch"><input type="checkbox" id="blinkToggle" /><span class="slider"></span></label><span class="active-state">On</span></div>
            </div>
            <div id="blinkPanel" class="blink-terminal">
              <div class="chip-matrix" id="blinkOptionsContainer" style="display: grid; grid-template-columns: repeat(2, 1fr); gap: 0.5rem; margin-bottom: 0.75rem;"></div>
              <div class="terminal-footer" style="display: block;">
                <button class="btn" id="selectBlinkBtn" style="background: var(--accent); color: var(--bg); font-weight: 700; border: none; width: 100%;">Trigger Action (Long Blink)</button>
                <div class="selection-focus" style="margin-top: 0.5rem">Focus: <span id="currentBlinkOption">Head Up</span></div>
              </div>
              <div style="display: flex; flex-direction: column; gap: 0.4rem; border-top: 1px solid var(--border); padding-top: 0.75rem; font-size: 0.7rem; color: var(--text-muted);">
                <div><strong>1 Short Blink:</strong> Next Item</div><div><strong>1 Long Blink:</strong> Select Item</div>
              </div>
            </div>
          </div>
        </div>
      </div>
    </div>
    <script>
      (() => {
        let headAngle = 0, footAngle = 0, leftTilt = 0, rightTilt = 0;
        const MAX_HEAD = 60, MAX_FOOT = 30, MAX_TILT = 25;
        let blinkActive = false, blinkTimer = null, blinkIndex = 0;
        const blinkOptions = ["Head Up", "Head Down", "Foot Up", "Foot Down", "Left Tilt Up", "Left Tilt Down", "Right Tilt Up", "Right Tilt Down"];
        
        const headSeg = document.getElementById("headSeg"), footSegContainer = document.getElementById("footSegContainer"), footFlapSeg = document.getElementById("footFlapSeg"), leftHalf = document.getElementById("leftHalf"), rightHalf = document.getElementById("rightHalf"), sagDisplay = document.getElementById("sagDisplay"), latDisplay = document.getElementById("latDisplay"), headPresetBtns = document.querySelectorAll("#headPresetGroup .btn.preset"), footPresetBtns = document.querySelectorAll("#footPresetGroup .btn.preset"), blinkToggle = document.getElementById("blinkToggle"), blinkPanel = document.getElementById("blinkPanel"), blinkOptionsContainer = document.getElementById("blinkOptionsContainer"), currentBlinkOptionSpan = document.getElementById("currentBlinkOption"), selectBlinkBtn = document.getElementById("selectBlinkBtn");

        function sendCmd(path) { fetch(path, { method: "GET", mode: "no-cors" }).catch(() => {}); }
        function refreshUI() {
          headSeg.style.transform = `rotate(${headAngle}deg)`; footSegContainer.style.transform = `rotate(${-footAngle}deg)`; footFlapSeg.style.transform = `rotate(${footAngle * 1.5}deg)`;
          sagDisplay.textContent = `Head ${headAngle}° · Foot ${footAngle}°`; leftHalf.style.transform = `rotate(${leftTilt}deg)`; rightHalf.style.transform = `rotate(${-rightTilt}deg)`;
          latDisplay.textContent = `Left ${leftTilt}° · Right ${rightTilt}°`;
          headPresetBtns.forEach(btn => btn.classList.toggle("active", headAngle === parseInt(btn.dataset.angle, 10)));
          footPresetBtns.forEach(btn => btn.classList.toggle("active", footAngle === parseInt(btn.dataset.angle, 10)));
          if (blinkActive) {
            document.querySelectorAll(".blink-chip").forEach((chip, idx) => chip.classList.toggle("current", idx === blinkIndex));
            currentBlinkOptionSpan.textContent = blinkOptions[blinkIndex];
          }
        }
        function setHeadPreset(angle) { removeDragClasses(); headAngle = angle; sendCmd(`/api?head=${headAngle}`); refreshUI(); }
        function setFootPreset(angle) { removeDragClasses(); footAngle = angle; sendCmd(`/api?foot=${footAngle}`); refreshUI(); }
        function moveSagittal(part, dir) {
          removeDragClasses();
          if (part === "head") { headAngle = dir === "up" ? Math.min(MAX_HEAD, headAngle + 5) : Math.max(0, headAngle - 5); sendCmd(`/api?head=${headAngle}`); }
          else { footAngle = dir === "up" ? Math.min(MAX_FOOT, footAngle + 5) : Math.max(0, footAngle - 5); sendCmd(`/api?foot=${footAngle}`); }
          refreshUI();
        }
        function moveLateral(side, dir) {
          removeDragClasses();
          if (side === "left") { if (rightTilt > 0) rightTilt = 0; leftTilt = dir === "up" ? Math.min(MAX_TILT, leftTilt + 5) : Math.max(0, leftTilt - 5); sendCmd(`/api?leftTilt=${leftTilt}`); }
          else { if (leftTilt > 0) leftTilt = 0; rightTilt = dir === "up" ? Math.min(MAX_TILT, rightTilt + 5) : Math.max(0, rightTilt - 5); sendCmd(`/api?rightTilt=${rightTilt}`); }
          refreshUI();
        }
        function removeDragClasses() { [headSeg, footSegContainer, footFlapSeg, leftHalf, rightHalf].forEach(el => el.classList.remove("dragging-active")); }
        function addDragClasses() { [headSeg, footSegContainer, footFlapSeg, leftHalf, rightHalf].forEach(el => el.classList.add("dragging-active")); }
        function executeBlinkOption(option) {
          switch (option) {
            case "Head Up": moveSagittal("head", "up"); break; case "Head Down": moveSagittal("head", "down"); break;
            case "Foot Up": moveSagittal("foot", "up"); break; case "Foot Down": moveSagittal("foot", "down"); break;
            case "Left Tilt Up": moveLateral("left", "up"); break; case "Left Tilt Down": moveLateral("left", "down"); break;
            case "Right Tilt Up": moveLateral("right", "up"); break; case "Right Tilt Down": moveLateral("right", "down"); break;
          }
        }
        function buildBlinkChips() {
          blinkOptionsContainer.innerHTML = "";
          blinkOptions.forEach((opt, idx) => {
            const chip = document.createElement("div"); chip.className = "blink-chip"; chip.textContent = opt;
            chip.addEventListener("click", () => { blinkIndex = idx; refreshUI(); executeBlinkOption(opt); });
            blinkOptionsContainer.appendChild(chip);
          });
        }
        function startBlinkSim() { if (blinkTimer) clearInterval(blinkTimer); blinkTimer = setInterval(() => { blinkIndex = (blinkIndex + 1) % blinkOptions.length; refreshUI(); }, 1800); }
        function stopBlinkSim() { if (blinkTimer) { clearInterval(blinkTimer); blinkTimer = null; } }
        function toggleBlink(enable) { blinkActive = enable; if (enable) { blinkPanel.classList.add("active"); buildBlinkChips(); refreshUI(); startBlinkSim(); } else { blinkPanel.classList.remove("active"); stopBlinkSim(); } }

        document.getElementById("headUpBtn").addEventListener("click", () => moveSagittal("head", "up"));
        document.getElementById("headDownBtn").addEventListener("click", () => moveSagittal("head", "down"));
        document.getElementById("footUpBtn").addEventListener("click", () => moveSagittal("foot", "up"));
        document.getElementById("footDownBtn").addEventListener("click", () => moveSagittal("foot", "down"));
        document.getElementById("leftUpBtn").addEventListener("click", () => moveLateral("left", "up"));
        document.getElementById("leftDownBtn").addEventListener("click", () => moveLateral("left", "down"));
        document.getElementById("rightUpBtn").addEventListener("click", () => moveLateral("right", "up"));
        document.getElementById("rightDownBtn").addEventListener("click", () => moveLateral("right", "down"));
        headPresetBtns.forEach(btn => btn.addEventListener("click", () => setHeadPreset(parseInt(btn.dataset.angle, 10))));
        footPresetBtns.forEach(btn => btn.addEventListener("click", () => setFootPreset(parseInt(btn.dataset.angle, 10))));
        blinkToggle.addEventListener("change", (e) => toggleBlink(e.target.checked));
        selectBlinkBtn.addEventListener("click", () => { if (blinkActive) executeBlinkOption(blinkOptions[blinkIndex]); });

        let isDragging = false, activePart = null, startY = 0, startAngle = 0;
        function getClientY(e) { return e.touches ? e.touches[0].clientY : e.clientY; }
        function handleDragStart(part, initialAngle, e) { isDragging = true; activePart = part; startY = getClientY(e); startAngle = initialAngle; addDragClasses(); }
        
        headSeg.addEventListener("mousedown", (e) => handleDragStart("head", headAngle, e));
        headSeg.addEventListener("touchstart", (e) => handleDragStart("head", headAngle, e));
        footSegContainer.addEventListener("mousedown", (e) => handleDragStart("foot", footAngle, e));
        footSegContainer.addEventListener("touchstart", (e) => handleDragStart("foot", footAngle, e));
        leftHalf.addEventListener("mousedown", (e) => handleDragStart("left", leftTilt, e));
        leftHalf.addEventListener("touchstart", (e) => handleDragStart("left", leftTilt, e));
        rightHalf.addEventListener("mousedown", (e) => handleDragStart("right", rightTilt, e));
        rightHalf.addEventListener("touchstart", (e) => handleDragStart("right", rightTilt, e));

        window.addEventListener("mousemove", handleDragMove);
        window.addEventListener("touchmove", handleDragMove, { passive: false });
        window.addEventListener("mouseup", handleDragEnd);
        window.addEventListener("touchend", handleDragEnd);

        function handleDragMove(e) {
          if (!isDragging) return; if (e.touches) e.preventDefault();
          const currentY = getClientY(e); const deltaY = startY - currentY; const sensitivity = 1.5; let computedAngle = startAngle + deltaY / sensitivity;
          if (activePart === "head") { headAngle = Math.max(0, Math.min(MAX_HEAD, Math.round(computedAngle))); sendCmd(`/api?head=${headAngle}`); }
          else if (activePart === "foot") { footAngle = Math.max(0, Math.min(MAX_FOOT, Math.round(computedAngle))); sendCmd(`/api?foot=${footAngle}`); }
          else if (activePart === "left") { if (rightTilt > 0) rightTilt = 0; leftTilt = Math.max(0, Math.min(MAX_TILT, Math.round(computedAngle))); sendCmd(`/api?leftTilt=${leftTilt}`); }
          else if (activePart === "right") { if (leftTilt > 0) leftTilt = 0; rightTilt = Math.max(0, Math.min(MAX_TILT, Math.round(computedAngle))); sendCmd(`/api?rightTilt=${rightTilt}`); }
          refreshUI();
        }
        function handleDragEnd() { if (isDragging) { isDragging = false; activePart = null; setTimeout(removeDragClasses, 50); } }
        removeDragClasses(); headAngle = 0; footAngle = 0; refreshUI(); window.addEventListener("beforeunload", stopBlinkSim);
      })();
    </script>
  </body>
</html>
)=====";

// Handler for the root web page URL "/"
void handleRoot() {
  server.send(200, "text/html", html_page);
}

// Handler to capture active slider/button URL changes
void handleAPI() {
  bool updated = false;

  if (server.hasArg("head")) {
    headAngle = server.arg("head").toInt();
    updated = true;
  }
  if (server.hasArg("foot")) {
    footAngle = server.arg("foot").toInt();
    updated = true;
  }
  if (server.hasArg("leftTilt")) {
    leftTilt = server.arg("leftTilt").toInt();
    if (leftTilt > 0) rightTilt = 0; // Balance constraint check
    updated = true;
  }
  if (server.hasArg("rightTilt")) {
    rightTilt = server.arg("rightTilt").toInt();
    if (rightTilt > 0) leftTilt = 0; // Balance constraint check
    updated = true;
  }

  // Print state shifts to the Serial Monitor immediately
  if (updated) {
    Serial.println("--- BED STATE UPDATE ---");
    Serial.printf("Head Angle : %d°\n", headAngle);
    Serial.printf("Foot Angle : %d°\n", footAngle);
    Serial.printf("Left Tilt  : %d°\n", leftTilt);
    Serial.printf("Right Tilt : %d°\n", rightTilt);
    Serial.println("------------------------\n");
  }

  // Respond to browser
  server.send(200, "text/plain", "OK");
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\nInitializing MediTilt Pro System...");

  // Boot up the WiFi in Access Point Mode
  WiFi.softAP(ssid, password);
  
  Serial.println("Access Point Started!");
  Serial.print("SSID: ");
  Serial.println(ssid);
  Serial.print("AP IP Address: ");
  Serial.println(WiFi.softAPIP());

  // Attach Web URL request handles
  server.on("/", handleRoot);
  server.on("/api", handleAPI);

  server.begin();
  Serial.println("HTTP Web Server Started ready for connections.");
}

void loop() {
  server.handleClient();
}