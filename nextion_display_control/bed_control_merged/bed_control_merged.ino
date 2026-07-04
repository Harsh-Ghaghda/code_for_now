#include <Wire.h>
#include <Arduino.h>
#include <Ticker.h>
#include "calibration_table.h" // Must be in the same directory

//========================================================================
// 0. BOARD AUTO-DETECTION (ESP32 vs ESP8266)
//========================================================================
// The two source sketches this was merged from assumed different boards:
// one used ESP32-style pin 36 / 12-bit ADC (0-4095) + Serial2, the other
// used ESP8266-style A0 / 10-bit ADC (0-1023) + ICACHE_RAM_ATTR.
// This block picks the right constants automatically so it compiles clean
// on whichever core you build for. If you know for certain which board
// you're on, you can delete the #if and hardcode the right branch.
#if defined(ESP32)
  #define ENCODER_PIN 36
  #define ADC_MAX 4095.0
  #define ENCODER_ISR_ATTR IRAM_ATTR
#else
  #define ENCODER_PIN A0
  #define ADC_MAX 1023.0
  #define ENCODER_ISR_ATTR ICACHE_RAM_ATTR
#endif

//========================================================================
// 1. UI & NEXTION CONFIGURATION
//========================================================================
int hoverColor = 1055;     // Custom Blue highlight
int defaultColor = 65535;  // Default normal button color (White)

int currentPage = 0;
int currentIndex = 0;

String page0_btns[] = {"b1", "b6", "b2", "b0"};
String page1_btns[] = {"b1", "b2", "b3", "b4", "b0"};
String page2_btns[] = {"b9", "b10", "b0"};

const int page0_size = 4;
const int page1_size = 5;
const int page2_size = 3;

//========================================================================
// 2. MPU6050 (IMU) CONFIGURATION
//========================================================================
#define MPU6050_ADDR 0x68

int16_t AcX, AcY, AcZ;
int16_t GyX, GyY, GyZ;

const float ACC_SCALE  = 16384.0;
const float GYRO_SCALE = 131.0;

float accOffsetX = 0, accOffsetY = 0, accOffsetZ = 0;
float gyroOffsetX = 0, gyroOffsetY = 0, gyroOffsetZ = 0;

float roll = 0;
float pitch = 0;
unsigned long lastMicros = 0;

class ComplementaryFilter {
  public:
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
// 3. AS5600 ENCODER CONFIGURATION (Ticker interrupt, 250Hz)
//========================================================================
const unsigned long ENCODER_SAMPLE_INTERVAL_MS = 4; // 250 Hz

// volatile because these are written from a timer interrupt (readEncoderISR)
// and read from the main loop - the compiler must not cache stale values.
//
// "rotations" is an ABSOLUTE position count: it only ever sits at 0 or
// above. Moving away from the flat/reference position increases it, moving
// back toward flat decreases it, and it's clamped so it can never go
// negative.
volatile long rotations = 0;
volatile float lastAngle = 0;

Ticker encoderTicker;

// Runs on a hardware timer, completely independent of loop() timing (IMU
// processing, Nextion/Serial handling, etc. can no longer starve this).
// Keep this short - no Serial prints, no String/malloc, no blocking calls.
void ENCODER_ISR_ATTR readEncoderISR() {
  int raw = analogRead(ENCODER_PIN);
  float angle = (raw / ADC_MAX) * 360.0;

  float diff = angle - lastAngle;

  // NOTE: which branch means "away from flat" vs "toward flat" depends on
  // your physical wiring/geometry. If your rotation count moves the wrong
  // way, swap the ++ and -- below.
  if (diff > 200.0) {
    rotations++;              // moving away from flat - grow the count
  }
  if (diff < -200.0) {
    rotations--;              // moving back toward flat
    if (rotations < 0) rotations = 0;  // clamp - stay on the positive scale
  }

  lastAngle = angle;
}

//========================================================================
// 4. CYTRON MOTOR DRIVER CONFIGURATION
//========================================================================
#define MOTOR_PWM_PIN 12
#define MOTOR_DIR_PIN 13

const int MOTOR_SPEED = 255;
long targetRotations = 0;
bool motorMoving = false;

void motorStop() { analogWrite(MOTOR_PWM_PIN, 0); }

// Drives the motor in whichever direction INCREASES the "rotations" count.
// If you type in a target and the bed tilts the wrong way, swap HIGH/LOW
// in these two functions (or swap the two motor wires on the Cytron output).
void motorDriveIncreasing() { digitalWrite(MOTOR_DIR_PIN, HIGH); analogWrite(MOTOR_PWM_PIN, MOTOR_SPEED); }
void motorDriveDecreasing() { digitalWrite(MOTOR_DIR_PIN, LOW); analogWrite(MOTOR_PWM_PIN, MOTOR_SPEED); }

void setupMotor() {
  pinMode(MOTOR_PWM_PIN, OUTPUT);
  pinMode(MOTOR_DIR_PIN, OUTPUT);
  motorStop();
}

// Converts an absolute rotation count to an interpolated roll angle, using
// the nonlinear calibration table. Values outside the calibrated range are
// clamped to the nearest table endpoint.
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
    bool inRange = increasing ? (targetRoll >= roll0 && targetRoll <= roll1) : (targetRoll <= roll0 && targetRoll >= roll1);

    if (inRange) {
      int32_t rot0 = pgm_read_dword(&calRotations[i]);
      int32_t rot1 = pgm_read_dword(&calRotations[i + 1]);
      float t = (roll1 == roll0) ? 0.0f : (targetRoll - roll0) / (roll1 - roll0);
      return rot0 + (long)lround(t * (rot1 - rot0));
    }
  }
  return pgm_read_dword(&calRotations[CAL_TABLE_SIZE - 1]);
}

// FIX: these two branches were swapped in the original code. The names
// "Increasing"/"Decreasing" refer to what happens to the ROTATION COUNT,
// not the target. So if we're currently above the target, we need to
// DRIVE DOWN (decrease) to reach it, and vice versa. The old code did the
// opposite, which is why the bed could climb but never come back down to
// a lower angle once it had gone up - each attempt to lower it just drove
// it further up instead.
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

void setBedTarget(float targetAngle) {
  targetRotations = rollToRotations(targetAngle);
  motorMoving = true;
  Serial.print("\n>>> New target angle: "); Serial.print(targetAngle, 2);
  Serial.print(" deg -> Target rotations: "); Serial.println(targetRotations);
}

//========================================================================
// 5. NEXTION NAVIGATION LOGIC
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
  Serial2.print("ref " + target);   // <-- forces the display to redraw
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

void interpretBedCommand(String cmd) {
  char foundCommand = ' ';
  for (int i = 0; i < cmd.length() - 1; i++) {
    if (cmd.charAt(i) == 'H') {
      foundCommand = cmd.charAt(i + 1);
      break;
    }
  }

  if (foundCommand == '0')      { setBedTarget(0.0); }
  else if (foundCommand == '1') { setBedTarget(15.0); }
  else if (foundCommand == '2') { setBedTarget(30.0); }
  else if (foundCommand == '3') { setBedTarget(60.0); }
  else {
    Serial.print("Command acknowledged. Length: ");
    Serial.println(cmd.length());
  }
}

//========================================================================
// 6. SENSOR SETUP HELPERS
//========================================================================
void setupMPU() {
  Wire.beginTransmission(MPU6050_ADDR); Wire.write(0x6B); Wire.write(0x00); Wire.endTransmission(true);
  Wire.beginTransmission(MPU6050_ADDR); Wire.write(0x1C); Wire.write(0x00); Wire.endTransmission(true);
  Wire.beginTransmission(MPU6050_ADDR); Wire.write(0x1B); Wire.write(0x00); Wire.endTransmission(true);
}

void readMPU() {
  Wire.beginTransmission(MPU6050_ADDR);
  Wire.write(0x3B);
  Wire.endTransmission(false);
  Wire.requestFrom(MPU6050_ADDR, 14, true);

  AcX = (Wire.read() << 8) | Wire.read();
  AcY = (Wire.read() << 8) | Wire.read();
  AcZ = (Wire.read() << 8) | Wire.read();
  Wire.read(); Wire.read();
  GyX = (Wire.read() << 8) | Wire.read();
  GyY = (Wire.read() << 8) | Wire.read();
  GyZ = (Wire.read() << 8) | Wire.read();
}

// Calibration (5 seconds) - GYRO ONLY. Accel offsets are explicitly kept
// at 0 here (or hardcode your known flat offsets if you have them).
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

  accOffsetX = 0;
  accOffsetY = 0;
  accOffsetZ = 0;

  Serial.print("Calibration Complete. Samples: ");
  Serial.println(samples);
}

//========================================================================
// 7. MAIN SETUP & LOOP
//========================================================================
void setup() {
  Serial.begin(115200);
  Serial2.begin(9600, SERIAL_8N1, 16, 17);

  Wire.begin(4, 5);
  setupMotor();
  setupMPU();
  delay(500);
  calibrateMPU();

  // Seed the filters with initial positioning
  readMPU();
  float ax = AcX - accOffsetX;
  float ay = AcY - accOffsetY;
  float az = AcZ - accOffsetZ;
  cFilterRoll.angle  = atan2(ay, az) * 180.0 / PI;
  cFilterPitch.angle = atan2(-ax, sqrt(ay * ay + az * az)) * 180.0 / PI;
  lastMicros = micros();

  // Seed the rotation count from the true physical starting angle so it
  // doesn't boot up thinking it's at 0 when it isn't.
  rotations = rollToRotations(cFilterRoll.angle);

  // Seed encoder's lastAngle so the first ISR tick doesn't register a
  // false wraparound, then start the high-frequency (250Hz) encoder
  // sampling on its own hardware timer - decoupled from the Nextion/Serial/
  // motor loop, so nothing in loop() can starve it or cause missed counts.
  int raw = analogRead(ENCODER_PIN);
  lastAngle = (raw / ADC_MAX) * 360.0;
  encoderTicker.attach_ms(ENCODER_SAMPLE_INTERVAL_MS, readEncoderISR);

  delay(500);
  clearAllButtonsOnPage();
  highlightCurrentButton(hoverColor);
}

void loop() {
  unsigned long currentMillis = millis();

  // 1. Process Live Blinks from Python via USB
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

  // 2. Process physical screen presses from Nextion
  if (Serial2.available() > 0) {
    String incomingCmd = Serial2.readStringUntil('\n');
    incomingCmd.trim();
    if (incomingCmd.length() > 0) {
      interpretBedCommand(incomingCmd);
    }
  }

  // 3. Strict 20ms IMU Sample Loop
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
  }

  // 4. Drive the motor toward active targets
  // (encoder sampling now runs entirely in the background via Ticker ISR -
  // no manual read needed here anymore)
  updateMotorControl();
}
