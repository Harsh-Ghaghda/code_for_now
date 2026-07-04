#include <Wire.h>
#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <WebSocketsServer.h>

//========================
// MPU6050 Config
//========================
#define MPU6050_ADDR 0x68

// Raw Sensor Variables
int16_t AcX, AcY, AcZ;
int16_t GyX, GyY, GyZ;

// Sensitivity Scale Factors
// Accel: ±2g range  -> 16384 LSB/g
// Gyro:  ±250 dps    -> 131 LSB/(deg/s)
const float ACC_SCALE  = 16384.0;
const float GYRO_SCALE = 131.0;

// Offsets
float accOffsetX = 0;
float accOffsetY = 0;
float accOffsetZ = 0;

float gyroOffsetX = 0;
float gyroOffsetY = 0;
float gyroOffsetZ = 0;

// Angles
float roll = 0;
float pitch = 0;

// Timing
unsigned long lastMicros = 0;

// Complementary Filter Class
class ComplementaryFilter
{
  public:
    // TUNING FACTOR 'alpha':
    // Closer to 1.0 = trust gyro more (smooth, but drifts over time).
    // Closer to 0.0 = trust accelerometer more (noisy, but no drift).
    float alpha = 0.98;
    float angle = 0.0;

    // gyroRate: deg/s from gyro (bias-corrected)
    // accAngle: angle computed from accelerometer (deg)
    // dt: elapsed time in seconds since last update
    float update(float gyroRate, float accAngle, float dt)
    {
      float gyroAngle = angle + gyroRate * dt;
      angle = alpha * gyroAngle + (1.0 - alpha) * accAngle;
      return angle;
    }
};

ComplementaryFilter cFilterRoll;
ComplementaryFilter cFilterPitch;

//========================
// AS5600 (Analog) Config
//========================
#define ENCODER_PIN A0

long rotations = 0;
float lastAngle = 0;

//========================
// WiFi AP + Live Web Dashboard (streams data, no flash writes)
//========================
// Connect your phone/laptop's WiFi to this network, then browse to
// http://192.168.4.1 to see live data and download it as CSV.
const char* AP_SSID = "RollRotationLogger";
const char* AP_PASS = "logger123";   // WPA2 requires 8+ characters

ESP8266WebServer webServer(80);
WebSocketsServer webSocket(81);

// The entire dashboard (HTML + CSS + JS) lives on the ESP8266 itself,
// since AP mode has no internet access - nothing is loaded from a CDN.
const char PAGE_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Roll / Rotation Live Logger</title>
<style>
  body { font-family: -apple-system, Arial, sans-serif; background:#111; color:#eee; margin:0; padding:16px; }
  h1 { font-size:20px; margin:0 0 4px; }
  .sub { color:#888; font-size:13px; margin-bottom:16px; }
  .cards { display:flex; gap:12px; margin-bottom:16px; flex-wrap:wrap; }
  .card { background:#1c1c1c; border-radius:10px; padding:14px 20px; flex:1; min-width:120px; }
  .card .label { color:#888; font-size:12px; text-transform:uppercase; }
  .card .value { font-size:28px; font-weight:600; margin-top:4px; }
  canvas { width:100%; height:160px; background:#1c1c1c; border-radius:10px; display:block; }
  table { width:100%; border-collapse:collapse; margin-top:14px; font-size:13px; }
  th, td { text-align:left; padding:4px 8px; border-bottom:1px solid #262626; }
  th { color:#888; font-weight:500; }
  #dlBtn, #clearBtn { background:#4f8cff; color:#fff; border:none; padding:10px 18px; border-radius:8px;
        font-size:14px; margin-top:14px; margin-right:8px; cursor:pointer; }
  #clearBtn { background:#333; }
  #status { font-size:12px; margin-left:8px; }
  .ok { color:#5fd97e; } .bad { color:#e05d5d; }
</style>
</head>
<body>
  <h1>Roll &amp; Rotation Live Logger</h1>
  <div class="sub">Streaming live over WebSocket from the ESP8266 &middot; <span id="status" class="bad">connecting...</span></div>

  <div class="cards">
    <div class="card"><div class="label">Roll (deg)</div><div class="value" id="rollVal">--</div></div>
    <div class="card"><div class="label">Rotations (abs)</div><div class="value" id="rotVal">--</div></div>
    <div class="card"><div class="label">Samples received</div><div class="value" id="countVal">0</div></div>
  </div>

  <canvas id="chart" width="600" height="160"></canvas>

  <button id="dlBtn">Download CSV</button>
  <button id="clearBtn">Clear Data</button>

  <table>
    <thead><tr><th>Millis</th><th>Rotations (abs)</th><th>Roll</th></tr></thead>
    <tbody id="rows"></tbody>
  </table>

<script>
  let history = [];   // {t, rot, roll} full log kept in the browser
  const maxRows = 15; // rows shown in the table (full history still kept for download)

  const statusEl = document.getElementById('status');
  const rollVal = document.getElementById('rollVal');
  const rotVal = document.getElementById('rotVal');
  const countVal = document.getElementById('countVal');
  const rowsEl = document.getElementById('rows');
  const canvas = document.getElementById('chart');
  const ctx = canvas.getContext('2d');

  function connect() {
    const ws = new WebSocket('ws://' + location.hostname + ':81/');
    ws.onopen = () => { statusEl.textContent = 'connected'; statusEl.className = 'ok'; };
    ws.onclose = () => { statusEl.textContent = 'disconnected - retrying...'; statusEl.className = 'bad'; setTimeout(connect, 1000); };
    ws.onerror = () => ws.close();
    ws.onmessage = (evt) => {
      // Expected format: "millis,abs_rotations,roll"
      const parts = evt.data.split(',');
      if (parts.length !== 3) return;
      const point = { t: parseInt(parts[0]), rot: parseInt(parts[1]), roll: parseFloat(parts[2]) };
      history.push(point);

      rollVal.textContent = point.roll.toFixed(2);
      rotVal.textContent = point.rot;
      countVal.textContent = history.length;

      const tr = document.createElement('tr');
      tr.innerHTML = `<td>${point.t}</td><td>${point.rot}</td><td>${point.roll.toFixed(2)}</td>`;
      rowsEl.prepend(tr);
      while (rowsEl.children.length > maxRows) rowsEl.removeChild(rowsEl.lastChild);

      drawChart();
    };
  }

  function drawChart() {
    const w = canvas.width, h = canvas.height;
    ctx.clearRect(0, 0, w, h);
    const data = history.slice(-200).map(p => p.roll);
    if (data.length < 2) return;
    const minV = Math.min(...data), maxV = Math.max(...data);
    const range = (maxV - minV) || 1;
    ctx.beginPath();
    data.forEach((v, i) => {
      const x = (i / (data.length - 1)) * w;
      const y = h - ((v - minV) / range) * (h - 10) - 5;
      if (i === 0) ctx.moveTo(x, y); else ctx.lineTo(x, y);
    });
    ctx.strokeStyle = '#4f8cff';
    ctx.lineWidth = 2;
    ctx.stroke();
  }

  document.getElementById('dlBtn').onclick = () => {
    let csv = 'millis,abs_rotations,roll\n';
    history.forEach(p => { csv += `${p.t},${p.rot},${p.roll.toFixed(2)}\n`; });
    const blob = new Blob([csv], { type: 'text/csv' });
    const a = document.createElement('a');
    a.href = URL.createObjectURL(blob);
    a.download = 'rollmap.csv';
    a.click();
  };

  document.getElementById('clearBtn').onclick = () => {
    history = [];
    rowsEl.innerHTML = '';
    countVal.textContent = '0';
  };

  connect();
</script>
</body>
</html>
)rawliteral";

void handleRoot()
{
  webServer.send_P(200, "text/html", PAGE_HTML);
}

// Called whenever a WebSocket client connects/disconnects/sends data.
// We only push data out, so we don't need to act on incoming messages.
void webSocketEvent(uint8_t clientId, WStype_t type, uint8_t * payload, size_t length)
{
  if (type == WStype_CONNECTED)
  {
    Serial.print("Web client connected, id=");
    Serial.println(clientId);
  }
  else if (type == WStype_DISCONNECTED)
  {
    Serial.print("Web client disconnected, id=");
    Serial.println(clientId);
  }
}

void setupWebDashboard()
{
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS);

  Serial.print("AP started. SSID: ");
  Serial.print(AP_SSID);
  Serial.print("  Password: ");
  Serial.println(AP_PASS);
  Serial.print("Browse to: http://");
  Serial.println(WiFi.softAPIP());

  webServer.on("/", handleRoot);
  webServer.begin();

  webSocket.begin();
  webSocket.onEvent(webSocketEvent);
}

// Sends one sample to every connected browser. Non-blocking - if no
// clients are connected this is effectively a no-op.
void broadcastDataPoint(unsigned long timestampMs, long absRotationCount, float rollAngle)
{
  char msg[48];
  snprintf(msg, sizeof(msg), "%lu,%ld,%.2f", timestampMs, absRotationCount, rollAngle);
  webSocket.broadcastTXT(msg);
}

//========================
// MPU6050 Initialization
//========================
void setupMPU()
{
  // Wake up the MPU6050 (Register 0x6B, clear sleep bit)
  Wire.beginTransmission(MPU6050_ADDR);
  Wire.write(0x6B);
  Wire.write(0x00);
  Wire.endTransmission(true);

  // Set accelerometer to ±2g (Register 0x1C)
  Wire.beginTransmission(MPU6050_ADDR);
  Wire.write(0x1C);
  Wire.write(0x00);
  Wire.endTransmission(true);

  // Set gyro to ±250 dps (Register 0x1B)
  Wire.beginTransmission(MPU6050_ADDR);
  Wire.write(0x1B);
  Wire.write(0x00);
  Wire.endTransmission(true);
}

//========================
// Read MPU6050
//========================
void readMPU()
{
  Wire.beginTransmission(MPU6050_ADDR);
  Wire.write(0x3B); // Data register start (ACCEL_XOUT_H)
  Wire.endTransmission(false);

  Wire.requestFrom(MPU6050_ADDR, 14, true);

  // MPU6050 is big-endian (High byte first, then Low byte)
  AcX = (Wire.read() << 8) | Wire.read();
  AcY = (Wire.read() << 8) | Wire.read();
  AcZ = (Wire.read() << 8) | Wire.read();
  Wire.read(); Wire.read(); // Skip temperature (2 bytes)
  GyX = (Wire.read() << 8) | Wire.read();
  GyY = (Wire.read() << 8) | Wire.read();
  GyZ = (Wire.read() << 8) | Wire.read();
}

//========================
// Calibration (5 seconds)
//========================
void calibrateMPU()
{
  Serial.println();
  Serial.println("Calibrating MPU6050... Keep platform perfectly still.");

  long ax = 0, ay = 0, az = 0;
  long gx = 0, gy = 0, gz = 0;

  const unsigned long calDuration = 5000; // 5 seconds
  const unsigned long sampleDelay = 5;    // ms between samples
  unsigned long startTime = millis();
  int samples = 0;

  while (millis() - startTime < calDuration)
  {
    readMPU();
    ax += AcX;
    ay += AcY;
    az += AcZ;
    gx += GyX;
    gy += GyY;
    gz += GyZ;
    samples++;

    if (samples % 100 == 0) Serial.print(".");
    delay(sampleDelay);
  }
  Serial.println();

  accOffsetX = (float)ax / samples;
  accOffsetY = (float)ay / samples;
  // ACC_SCALE corresponds to 1g of gravity vector at baseline flat position
  accOffsetZ = ((float)az / samples) - ACC_SCALE;

  gyroOffsetX = (float)gx / samples;
  gyroOffsetY = (float)gy / samples;
  gyroOffsetZ = (float)gz / samples;

  Serial.print("Calibration Complete. Samples: ");
  Serial.println(samples);
}

//========================
// AS5600 Read (Analog)
//========================
void readEncoder()
{
  int raw = analogRead(ENCODER_PIN);
  float angle = (raw / 1023.0) * 360.0;

  float diff = angle - lastAngle;

  if (diff < -200.0) {
    rotations++;
  }
  if (diff > 200.0) {
    rotations--;
  }

  lastAngle = angle;
}

//========================
// Setup
//========================
void setup()
{
  Serial.begin(9600);

  Wire.begin(4, 5); // SDA=D2 (GPIO4), SCL=D1 (GPIO5)

  // Start the WiFi Access Point + web dashboard. Connect to the AP_SSID
  // network from your phone/laptop, then browse to the printed IP address.
  setupWebDashboard();

  setupMPU();
  delay(500);
  calibrateMPU();

  // Seed the filters with initial flat positioning
  readMPU();
  float ax = AcX - accOffsetX;
  float ay = AcY - accOffsetY;
  float az = AcZ - accOffsetZ;

  cFilterRoll.angle  = atan2(ay, az) * 180.0 / PI;
  cFilterPitch.angle = atan2(-ax, sqrt(ay * ay + az * az)) * 180.0 / PI;

  lastMicros = micros();

  // Seed encoder's lastAngle so the first loop iteration doesn't
  // register a false wraparound
  int raw = analogRead(ENCODER_PIN);
  lastAngle = (raw / 1023.0) * 360.0;

  Serial.println("MPU6050 + AS5600 running side by side.");
  Serial.println("Connect to the WiFi AP above, then open the printed IP in a browser.");
}

//========================
// Loop
//========================
void loop()
{
  //----- MPU6050 -----
  // Always service the web server + WebSocket, every single loop pass,
  // so the dashboard stays responsive regardless of sensor timing.
  webServer.handleClient();
  webSocket.loop();

  // Run the sensor read / filter / broadcast block on a strict 20ms
  // cadence, without blocking the web server via delay().
  static unsigned long lastSampleMillis = 0;
  unsigned long nowMs = millis();
  if (nowMs - lastSampleMillis < 20)
  {
    return; // not time for the next sample yet - keep servicing the web stack
  }
  lastSampleMillis = nowMs;

  //----- MPU6050 -----
  readMPU();

  // Compute elapsed time since last update
  unsigned long now = micros();
  float dt = (now - lastMicros) / 1000000.0;
  lastMicros = now;

  // Strip out zero-point calibration offsets (accel)
  float ax = AcX - accOffsetX;
  float ay = AcY - accOffsetY;
  float az = AcZ - accOffsetZ;

  // Strip out zero-rate offsets and convert gyro to deg/s
  float gxRate = (GyX - gyroOffsetX) / GYRO_SCALE;
  float gyRate = (GyY - gyroOffsetY) / GYRO_SCALE;

  // Convert raw gravity vectors into physical geometric angles
  float rollAcc  = atan2(ay, az) * 180.0 / PI;
  float pitchAcc = atan2(-ax, sqrt(ay * ay + az * az)) * 180.0 / PI;

  // Fuse gyro rate + accel angle via complementary filter
  // Note: roll relates to rotation about X axis -> use gyro X rate
  //       pitch relates to rotation about Y axis -> use gyro Y rate
  roll  = cFilterRoll.update(gxRate, rollAcc, dt);
  pitch = cFilterPitch.update(gyRate, pitchAcc, dt);

  //----- AS5600 (Analog) -----
  readEncoder();

  // Absolute rotation count: a rotation is a rotation regardless of direction,
  // so -3 and +3 both log/report as 3.
  long absRotations = labs(rotations);

  //----- Stream this sample to every connected browser -----
  broadcastDataPoint(nowMs, absRotations, roll);

  //----- Print combined output -----
  Serial.print("Roll: ");     Serial.print(roll, 2);
  Serial.print(" \tPitch: "); Serial.print(pitch, 2);
  Serial.print(" \tEncAngle: "); Serial.print(lastAngle, 2);
  Serial.print(" \tRotations(abs): "); Serial.println(absRotations);
}
