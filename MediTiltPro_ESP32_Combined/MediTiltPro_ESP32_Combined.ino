/*
==========================================================================
 MediTilt Pro - Combined ESP32 Firmware
==========================================================================
 Single ESP32 sketch that merges THREE previously separate control paths
 into one shared motor/IMU/encoder core:

   1) WEB DASHBOARD   - ESP32 WiFi Access Point + web server ("MediTilt Pro"
                         page). Slider/buttons on the page set a target head
                         angle over HTTP.
   2) BLINK CONTROL   - USB Serial link to the PC-side MediaPipe eye-blink
                         script (code.py). Single bytes '0' (short blink /
                         "next") and '1' (long blink / "select") drive
                         on-screen navigation.
   3) NEXTION DISPLAY - Physical Nextion touch panel on Serial2. Button
                         presses ("H0".."H3") on the panel set preset bed
                         angles directly, and the blink-based cursor
                         highlights/clicks the same Nextion buttons.

 All three paths ultimately call the SAME setTargetAngle() function, which
 arms the shared motor control loop - so a target set from the website, a
 blink selection, or a physical panel press all drive the bed identically.

 PORTED FROM ESP8266 TO ESP32:
   - ESP8266WiFi.h / ESP8266WebServer.h -> WiFi.h / WebServer.h
   - ESP8266WebServer server(80)        -> WebServer server(80)
   - WiFi.setSleepMode(WIFI_NONE_SLEEP) -> WiFi.setSleep(false)
   - AS5600 analog read now scaled by 4095.0 (ESP32's 12-bit ADC) instead
     of 1023.0 (ESP8266's 10-bit ADC)
   - ICACHE_RAM_ATTR -> IRAM_ATTR (only relevant if you reintroduce an ISR)

 ENCODER SAMPLING - DELIBERATELY NOT ON A Ticker/ISR:
   The original ESP8266 web-server sketch sampled the AS5600 from a
   Ticker ISR (readEncoderISR). On ESP32 this is unsafe: analogRead() on
   ESP32 uses the ADC driver, which is not interrupt-safe to call from a
   hardware timer callback and can hang or crash the chip. The Nextion/
   blink sketch already discovered this the hard way and switched to a
   plain non-blocking poll in loop() every 4ms instead - this combined
   sketch keeps that fix rather than reintroducing the Ticker.

 CALIBRATION ON BOOT:
   calibrateMPU() now always runs a full 5-second stationary gyro
   calibration on every boot (the more thorough routine from the
   web-server sketch), regardless of which control path is used
   afterwards.

 REQUIRED COMPANION FILE:
   calibration_table.h must sit next to this .ino (unchanged - it is not
   regenerated here). It supplies calRoll[], calRotations[], CAL_TABLE_SIZE.
==========================================================================
*/

#include <WiFi.h>
#include <WebServer.h>
#include <Wire.h>
#include <Arduino.h>

//========================================================================
// 0. WI-FI ACCESS POINT CONFIG
//========================================================================
const char* ssid     = "MediTilt_Pro_Net";
const char* password = "clinicalcontrol"; // must be at least 8 characters

WebServer server(80);

// Bed state variables (website-facing). Only headAngle currently drives a
// physical motor (see setTargetAngle()); footAngle/leftTilt/rightTilt are
// kept as UI state for the web GUI / future actuators.
int headAngle  = 0;
int footAngle  = 0;
int leftTilt   = 0;
int rightTilt  = 0;

// The raw HTML/CSS/JS for the web dashboard (unchanged from the ESP8266 version)
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

//========================================================================
// 1. NEXTION UI CONFIGURATION (blink-cursor navigation on the panel)
//========================================================================
int hoverColor   = 1055;   // Custom blue highlight
int defaultColor = 65535;  // Default normal button color (white)

int currentPage  = 0;
int currentIndex = 0;

String page0_btns[] = {"b1", "b6", "b2", "b0"};
String page1_btns[] = {"b1", "b2", "b3", "b4", "b0"};
String page2_btns[] = {"b9", "b10", "b0"};

const int page0_size = 4;
const int page1_size = 5;
const int page2_size = 3;

//========================================================================
// 2. MPU6050 (IMU) CONFIGURATION - shared by all three control paths
//========================================================================
#define MPU6050_ADDR 0x68

int16_t AcX, AcY, AcZ;
int16_t GyX, GyY, GyZ;

const float ACC_SCALE  = 16384.0; // +-2g range
const float GYRO_SCALE = 131.0;   // +-250 dps range

float accOffsetX = 0, accOffsetY = 0, accOffsetZ = 0;
float gyroOffsetX = 0, gyroOffsetY = 0, gyroOffsetZ = 0;

float roll  = 0;
float pitch = 0;
unsigned long lastMicros = 0;

class ComplementaryFilter {
  public:
    // Closer to 1.0 = trust gyro more (smooth, but drifts).
    // Closer to 0.0 = trust accel more (noisy, but no drift).
    float alpha = 0.98;
    float angle = 0.0;
    float update(float gyroRate, float accAngle, float dt) {
      float gyroAngle = angle + gyroRate * dt;
      angle = alpha * gyroAngle + (1.0 - alpha) * accAngle;
      return angle;
    }
};

ComplementaryFilter cFilterRoll;
ComplementaryFilter cFilterPitch;

//========================================================================
// 3. AS5600 ENCODER (ESP32 ADC1, GPIO36 - polled, NOT on a Ticker ISR)
//========================================================================
#define ENCODER_PIN 36
const unsigned long ENCODER_SAMPLE_INTERVAL_MS = 4;
unsigned long lastEncoderMillis = 0;

volatile long rotations = 0;   // absolute position count, clamped >= 0
float lastAngle = 0;

void readEncoder() {
  int raw = analogRead(ENCODER_PIN);
  float angle = (raw / 4095.0) * 360.0; // ESP32 = 12-bit ADC

  float diff = angle - lastAngle;

  // Which branch means "away from flat" vs "toward flat" depends on your
  // physical wiring/geometry - matches the direction observed in
  // rollmap.csv. Swap ++ and -- below if your count moves the wrong way.
  if (diff > 200.0) {
    rotations++;
  }
  if (diff < -200.0) {
    rotations--;
    if (rotations < 0) rotations = 0;
  }
  lastAngle = angle;
}

//========================================================================
// 4. CYTRON MD30C R2 MOTOR DRIVER (sign-magnitude PWM+DIR, soft-start ramp)
//========================================================================
#include "calibration_table.h" // Must sit next to this .ino

#define MOTOR_PWM_PIN 12 // Cytron PWM input (speed)
#define MOTOR_DIR_PIN 13 // Cytron DIR input (direction)

const int MOTOR_SPEED = 255; // 0-255 PWM ceiling - tune to a safe speed

long targetRotations = 0;
bool motorMoving = false;

// Soft-start: ramp PWM up gradually instead of jumping straight to
// MOTOR_SPEED, so the motor's inrush current doesn't brown out the board.
int currentMotorPWM = 0;
const int MOTOR_RAMP_STEP = 15; // PWM increment per 20ms control cycle

void motorStop() {
  analogWrite(MOTOR_PWM_PIN, 0);
  currentMotorPWM = 0;
}

void motorDriveIncreasing() {
  digitalWrite(MOTOR_DIR_PIN, HIGH);
  currentMotorPWM = min(currentMotorPWM + MOTOR_RAMP_STEP, MOTOR_SPEED);
  analogWrite(MOTOR_PWM_PIN, currentMotorPWM);
}

void motorDriveDecreasing() {
  digitalWrite(MOTOR_DIR_PIN, LOW);
  currentMotorPWM = min(currentMotorPWM + MOTOR_RAMP_STEP, MOTOR_SPEED);
  analogWrite(MOTOR_PWM_PIN, currentMotorPWM);
}

void setupMotor() {
  pinMode(MOTOR_PWM_PIN, OUTPUT);
  pinMode(MOTOR_DIR_PIN, OUTPUT);
  motorStop();
}

// Absolute rotation count -> interpolated roll angle (nonlinear calibration table)
float rotationsToRoll(long rot) {
  int32_t rotFirst = pgm_read_dword(&calRotations[0]);
  int32_t rotLast  = pgm_read_dword(&calRotations[CAL_TABLE_SIZE - 1]);

  if (rot <= rotFirst) return pgm_read_float(&calRoll[0]);
  if (rot >= rotLast)  return pgm_read_float(&calRoll[CAL_TABLE_SIZE - 1]);

  for (int i = 0; i < CAL_TABLE_SIZE - 1; i++) {
    int32_t r0 = pgm_read_dword(&calRotations[i]);
    int32_t r1 = pgm_read_dword(&calRotations[i + 1]);
    if (rot >= r0 && rot <= r1) {
      float roll0 = pgm_read_float(&calRoll[i]);
      float roll1 = pgm_read_float(&calRoll[i + 1]);
      float t = (r1 == r0) ? 0.0f : (float)(rot - r0) / (float)(r1 - r0);
      return roll0 + t * (roll1 - roll0);
    }
  }
  return pgm_read_float(&calRoll[CAL_TABLE_SIZE - 1]); // shouldn't reach here
}

// Target roll angle -> interpolated rotation count (inverse lookup)
long rollToRotations(float targetRoll) {
  float rollFirst = pgm_read_float(&calRoll[0]);
  float rollLast  = pgm_read_float(&calRoll[CAL_TABLE_SIZE - 1]);
  bool increasing = (rollLast >= rollFirst);

  if (increasing) {
    if (targetRoll <= rollFirst) return pgm_read_dword(&calRotations[0]);
    if (targetRoll >= rollLast)  return pgm_read_dword(&calRotations[CAL_TABLE_SIZE - 1]);
  } else {
    if (targetRoll >= rollFirst) return pgm_read_dword(&calRotations[0]);
    if (targetRoll <= rollLast)  return pgm_read_dword(&calRotations[CAL_TABLE_SIZE - 1]);
  }

  for (int i = 0; i < CAL_TABLE_SIZE - 1; i++) {
    float roll0 = pgm_read_float(&calRoll[i]);
    float roll1 = pgm_read_float(&calRoll[i + 1]);
    bool inRange = increasing
      ? (targetRoll >= roll0 && targetRoll <= roll1)
      : (targetRoll <= roll0 && targetRoll >= roll1);

    if (inRange) {
      int32_t rot0 = pgm_read_dword(&calRotations[i]);
      int32_t rot1 = pgm_read_dword(&calRotations[i + 1]);
      float t = (roll1 == roll0) ? 0.0f : (targetRoll - roll0) / (roll1 - roll0);
      return rot0 + (long)lround(t * (rot1 - rot0));
    }
  }
  return pgm_read_dword(&calRotations[CAL_TABLE_SIZE - 1]); // shouldn't reach here
}

// Single shared entry point for arming a new move - called from the web
// GUI (handleAPI), the Nextion panel (interpretBedCommand), AND the blink
// cursor's Nextion "click" (indirectly, since blink-select just clicks the
// same panel buttons). "source" is just a label for the Serial log.
void setTargetAngle(float targetAngle, const char* source) {
  targetRotations = rollToRotations(targetAngle);
  motorMoving = true;

  Serial.print("\n>>> [");
  Serial.print(source);
  Serial.print("] New target angle: ");
  Serial.print(targetAngle, 2);
  Serial.print(" deg -> target rotations: ");
  Serial.println(targetRotations);
}

// Call once per 20ms sensor sample: steps the motor toward targetRotations
// and stops exactly when the encoder count matches it.
void updateMotorControl() {
  if (!motorMoving) return;

  if (rotations > targetRotations) {
    motorDriveIncreasing();
  } else if (rotations < targetRotations) {
    motorDriveDecreasing();
  } else {
    motorStop();
    motorMoving = false;
    Serial.print("\n[SUCCESS] Target reached. Rotations: "); Serial.print(rotations);
    Serial.print(" | Active Roll: "); Serial.println(roll, 2);
  }
}

//========================================================================
// 5. NEXTION NAVIGATION LOGIC (blink-driven cursor + physical panel input)
//========================================================================
String getCurrentButtonID() {
  if (currentPage == 0) return page0_btns[currentIndex];
  if (currentPage == 1) return page1_btns[currentIndex];
  if (currentPage == 2) return page2_btns[currentIndex];
  return "";
}

void endNextionCmd() {
  Serial2.write(0xFF); Serial2.write(0xFF); Serial2.write(0xFF);
}

void highlightCurrentButton(int colorCode) {
  String target = getCurrentButtonID();
  Serial2.print(target + ".bco=" + String(colorCode));
  endNextionCmd();
  Serial2.print("ref " + target); // forces the display to redraw
  endNextionCmd();
}

void clearAllButtonsOnPage() {
  if (currentPage == 0) {
    for (int i = 0; i < page0_size; i++) { Serial2.print(page0_btns[i] + ".bco=" + String(defaultColor)); endNextionCmd(); }
  } else if (currentPage == 1) {
    for (int i = 0; i < page1_size; i++) { Serial2.print(page1_btns[i] + ".bco=" + String(defaultColor)); endNextionCmd(); }
  } else if (currentPage == 2) {
    for (int i = 0; i < page2_size; i++) { Serial2.print(page2_btns[i] + ".bco=" + String(defaultColor)); endNextionCmd(); }
  }
}

void handleNext() {
  highlightCurrentButton(defaultColor);
  currentIndex++;

  if (currentPage == 0 && currentIndex >= page0_size) currentIndex = 0;
  if (currentPage == 1 && currentIndex >= page1_size) currentIndex = 0;
  if (currentPage == 2 && currentIndex >= page2_size) currentIndex = 0;

  highlightCurrentButton(hoverColor);
}

void handleSelect() {
  String target = getCurrentButtonID();
  highlightCurrentButton(defaultColor);

  Serial2.print("click " + target + ",1"); endNextionCmd();
  delay(100);
  Serial2.print("click " + target + ",0"); endNextionCmd();

  bool pageChanged = false;
  int nextExecutionPage = currentPage;

  if (currentPage == 0) {
    if (target == "b1" || target == "b2") { nextExecutionPage = 1; pageChanged = true; }
    else if (target == "b0" || target == "b6") { nextExecutionPage = 2; pageChanged = true; }
  } else if (currentPage == 1 || currentPage == 2) {
    if (target == "b0") { nextExecutionPage = 0; pageChanged = true; }
  }

  if (pageChanged) {
    Serial2.print("page page" + String(nextExecutionPage));
    endNextionCmd();

    delay(250);
    currentPage = nextExecutionPage;
    currentIndex = 0;
    clearAllButtonsOnPage();
    highlightCurrentButton(hoverColor);
  } else {
    highlightCurrentButton(hoverColor);
  }
}

// Parses "H<n>" commands coming FROM the physical Nextion panel (its touch
// events are configured to print e.g. "H0", "H1", "H2", "H3" over Serial2)
// and maps them to preset bed angles via the shared setTargetAngle().
void interpretBedCommand(String cmd) {
  char foundCommand = ' ';
  for (int i = 0; i < cmd.length() - 1; i++) {
    if (cmd.charAt(i) == 'H') {
      foundCommand = cmd.charAt(i + 1);
      break;
    }
  }

  if (foundCommand == '0')      { setTargetAngle(0.0,  "Nextion"); }
  else if (foundCommand == '1') { setTargetAngle(15.0, "Nextion"); }
  else if (foundCommand == '2') { setTargetAngle(30.0, "Nextion"); }
  else if (foundCommand == '3') { setTargetAngle(60.0, "Nextion"); }
  else {
    Serial.print("Command acknowledged. Length: ");
    Serial.println(cmd.length());
  }
}

//========================================================================
// 6. WEB HANDLERS (dashboard hosted on the ESP32 Access Point)
//========================================================================
void handleRoot() {
  server.send(200, "text/html", html_page);
}

// "head" is wired straight into the motor: any change from the website
// arms a new target the same way a Nextion preset or blink-select does.
void handleAPI() {
  bool updated = false;

  if (server.hasArg("head")) {
    headAngle = server.arg("head").toInt();
    updated = true;
    setTargetAngle((float)headAngle, "Web");
  }
  if (server.hasArg("foot")) {
    footAngle = server.arg("foot").toInt();
    updated = true;
  }
  if (server.hasArg("leftTilt")) {
    leftTilt = server.arg("leftTilt").toInt();
    if (leftTilt > 0) rightTilt = 0; // balance constraint
    updated = true;
  }
  if (server.hasArg("rightTilt")) {
    rightTilt = server.arg("rightTilt").toInt();
    if (rightTilt > 0) leftTilt = 0; // balance constraint
    updated = true;
  }

  if (updated) {
    Serial.println("--- BED STATE UPDATE ---");
    Serial.printf("Head Angle : %d\xC2\xB0\n", headAngle);
    Serial.printf("Foot Angle : %d\xC2\xB0\n", footAngle);
    Serial.printf("Left Tilt  : %d\xC2\xB0\n", leftTilt);
    Serial.printf("Right Tilt : %d\xC2\xB0\n", rightTilt);
    Serial.println("------------------------\n");
  }

  server.send(200, "text/plain", "OK");
}

//========================================================================
// 7. MPU6050 SETUP / READ / CALIBRATION HELPERS
//========================================================================
void mpuWriteReg(uint8_t reg, uint8_t value) {
  Wire.beginTransmission(MPU6050_ADDR);
  Wire.write(reg);
  Wire.write(value);
  Wire.endTransmission(true);
}

void setupMPU() {
  mpuWriteReg(0x6B, 0x00); // wake up (clear sleep bit)
  mpuWriteReg(0x1C, 0x00); // accel range: +-2g
  mpuWriteReg(0x1B, 0x00); // gyro range: +-250 dps
}

void readMPU() {
  Wire.beginTransmission(MPU6050_ADDR);
  Wire.write(0x3B); // ACCEL_XOUT_H
  Wire.endTransmission(false);
  Wire.requestFrom(MPU6050_ADDR, 14, true);

  AcX = (Wire.read() << 8) | Wire.read();
  AcY = (Wire.read() << 8) | Wire.read();
  AcZ = (Wire.read() << 8) | Wire.read();
  Wire.read(); Wire.read(); // skip temperature
  GyX = (Wire.read() << 8) | Wire.read();
  GyY = (Wire.read() << 8) | Wire.read();
  GyZ = (Wire.read() << 8) | Wire.read();
}

// Full 5-second stationary gyro calibration - ALWAYS runs on boot,
// regardless of which control path (web/blink/Nextion) is used afterwards.
void calibrateMPU() {
  Serial.println();
  Serial.println("Calibrating MPU6050 Gyro... Keep platform perfectly still.");

  long gx = 0, gy = 0, gz = 0;
  const unsigned long calDuration = 5000; // 5 seconds
  const unsigned long sampleDelay = 5;    // ms between samples
  unsigned long startTime = millis();
  int samples = 0;

  while (millis() - startTime < calDuration) {
    readMPU();
    gx += GyX; gy += GyY; gz += GyZ;
    samples++;
    if (samples % 100 == 0) Serial.print(".");
    delay(sampleDelay);
  }
  Serial.println();

  gyroOffsetX = (float)gx / samples;
  gyroOffsetY = (float)gy / samples;
  gyroOffsetZ = (float)gz / samples;

  // Accel offsets left at 0 (hardcode known flat offsets here if you have them)
  accOffsetX = 0; accOffsetY = 0; accOffsetZ = 0;

  Serial.print("Calibration Complete. Samples: ");
  Serial.println(samples);
}

//========================================================================
// 8. SETUP
//========================================================================
void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\nInitializing MediTilt Pro System (ESP32)...");

  Serial2.begin(9600, SERIAL_8N1, 16, 17); // Nextion: RX=16, TX=17

  // --- WiFi Access Point ---
  WiFi.disconnect(true);  // forget any old STA credentials
  WiFi.mode(WIFI_OFF);
  delay(100);
  WiFi.mode(WIFI_AP);
  WiFi.setSleep(false);   // ESP32 equivalent of ESP8266's setSleepMode(WIFI_NONE_SLEEP)

  WiFi.softAP(ssid, password, 6, false, 4); // channel 6, not hidden, up to 4 clients

  Serial.println("Access Point Started!");
  Serial.print("SSID: "); Serial.println(ssid);
  Serial.print("AP IP Address: "); Serial.println(WiFi.softAPIP());

  // --- I2C / Motor / IMU ---
  Wire.begin(4, 5); // SDA=GPIO4, SCL=GPIO5
  setupMotor();
  setupMPU();
  delay(500);
  calibrateMPU();

  // Seed the filters with the true starting orientation
  readMPU();
  float ax = AcX - accOffsetX;
  float ay = AcY - accOffsetY;
  float az = AcZ - accOffsetZ;
  cFilterRoll.angle  = atan2(ay, az) * 180.0 / PI;
  cFilterPitch.angle = atan2(-ax, sqrt(ay * ay + az * az)) * 180.0 / PI;
  lastMicros = micros();

  // Seed the encoder count based on the true physical starting angle
  rotations = rollToRotations(cFilterRoll.angle);

  // Seed lastAngle so the first sample doesn't register a false wraparound
  int raw = analogRead(ENCODER_PIN);
  lastAngle = (raw / 4095.0) * 360.0;

  // --- Web server routes ---
  server.on("/", handleRoot);
  server.on("/api", handleAPI);
  server.begin();
  Serial.println("HTTP Web Server Started, ready for connections.");

  // --- Nextion panel initial state ---
  delay(500);
  clearAllButtonsOnPage();
  highlightCurrentButton(hoverColor);
}

//========================================================================
// 9. LOOP
//========================================================================
void loop() {
  unsigned long currentMillis = millis();

  // Service web clients every iteration so the dashboard stays responsive.
  server.handleClient();

  // 1. Non-blocking AS5600 sample (polled, not on a Ticker ISR - see header note)
  if (currentMillis - lastEncoderMillis >= ENCODER_SAMPLE_INTERVAL_MS) {
    lastEncoderMillis = currentMillis;
    readEncoder();
  }

  // 2. BLINK CONTROL - single bytes from the PC's MediaPipe script (code.py)
  //    over USB Serial: '0' = short blink (next), '1' = long blink (select)
  if (Serial.available() > 0) {
    char incomingChar = Serial.read();
    Serial.print("[USB RX] Got byte: "); Serial.println(incomingChar);
    if (incomingChar == '0') {
      handleNext();
      Serial.println("ACK:0");
    } else if (incomingChar == '1') {
      handleSelect();
      Serial.println("ACK:1");
    }
  }

  // 3. NEXTION PANEL - physical touch presses reported back over Serial2
  if (Serial2.available() > 0) {
    String incomingCmd = Serial2.readStringUntil('\n');
    incomingCmd.trim();
    if (incomingCmd.length() > 0) {
      interpretBedCommand(incomingCmd);
    }
  }

  // 4. Strict 20ms IMU sample + filter update
  static unsigned long lastSampleMillis = 0;
  if (currentMillis - lastSampleMillis >= 20) {
    lastSampleMillis = currentMillis;

    readMPU();
    unsigned long now = micros();
    float dt = (now - lastMicros) / 1000000.0;
    lastMicros = now;

    float ax = AcX - accOffsetX; float ay = AcY - accOffsetY; float az = AcZ - accOffsetZ;
    float gxRate = (GyX - gyroOffsetX) / GYRO_SCALE;
    float gyRate = (GyY - gyroOffsetY) / GYRO_SCALE;

    float rollAcc  = atan2(ay, az) * 180.0 / PI;
    float pitchAcc = atan2(-ax, sqrt(ay * ay + az * az)) * 180.0 / PI;

    roll  = cFilterRoll.update(gxRate, rollAcc, dt);
    pitch = cFilterPitch.update(gyRate, pitchAcc, dt);

    // 5. Drive the motor toward the active target, once per IMU sample
    updateMotorControl();
  }
}
