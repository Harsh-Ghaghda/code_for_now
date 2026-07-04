#include <Wire.h>
#include <Arduino.h>
#include "calibration_table.h" // Must be in the same directory

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
// 3. AS5600 ENCODER CONFIGURATION (FIXED: Removed Ticker Interrupts)
//========================================================================
#define ENCODER_PIN 36 
const unsigned long ENCODER_SAMPLE_INTERVAL_MS = 4;
unsigned long lastEncoderMillis = 0;

volatile long rotations = 0;
float lastAngle = 0;

void readEncoder() {
  int raw = analogRead(ENCODER_PIN);
  float angle = (raw / 4095.0) * 360.0; 
  float diff = angle - lastAngle;

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
// 4. CYTRON MOTOR DRIVER CONFIGURATION
//========================================================================
#define MOTOR_PWM_PIN 12 
#define MOTOR_DIR_PIN 13 

const int MOTOR_SPEED = 255; 
long targetRotations = 0;
bool motorMoving = false;

void motorStop() { analogWrite(MOTOR_PWM_PIN, 0); }
void motorDriveIncreasing() { digitalWrite(MOTOR_DIR_PIN, HIGH); analogWrite(MOTOR_PWM_PIN, MOTOR_SPEED); }
void motorDriveDecreasing() { digitalWrite(MOTOR_DIR_PIN, LOW); analogWrite(MOTOR_PWM_PIN, MOTOR_SPEED); }

void setupMotor() {
  pinMode(MOTOR_PWM_PIN, OUTPUT);
  pinMode(MOTOR_DIR_PIN, OUTPUT);
  motorStop();
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

void updateMotorControl() {
  if (!motorMoving) return;

  // Moves TOWARD the target safely
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

void calibrateMPU() {
  Serial.println("Calibrating MPU6050 Gyro...");
  long gx = 0, gy = 0, gz = 0;
  unsigned long startTime = millis();
  int samples = 0;
  
  while (millis() - startTime < 3000) {
    readMPU();
    gx += GyX; gy += GyY; gz += GyZ;
    samples++;
    delay(5);
  }
  
  gyroOffsetX = (float)gx / samples;
  gyroOffsetY = (float)gy / samples;
  gyroOffsetZ = (float)gz / samples;
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
  delay(200);
  calibrateMPU();

  readMPU();
  float ax = AcX - accOffsetX;
  float ay = AcY - accOffsetY;
  float az = AcZ - accOffsetZ;
  cFilterRoll.angle  = atan2(ay, az) * 180.0 / PI;
  cFilterPitch.angle = atan2(-ax, sqrt(ay * ay + az * az)) * 180.0 / PI;
  lastMicros = micros();

  rotations = rollToRotations(cFilterRoll.angle);

  int raw = analogRead(ENCODER_PIN);
  lastAngle = (raw / 4095.0) * 360.0; 

  delay(500);
  clearAllButtonsOnPage();
  highlightCurrentButton(hoverColor);
}

void loop() {
  unsigned long currentMillis = millis();

  // 1. Safe non-blocking sample for AS5600 encoder (Replaces Ticker)
  if (currentMillis - lastEncoderMillis >= ENCODER_SAMPLE_INTERVAL_MS) {
    lastEncoderMillis = currentMillis;
    readEncoder();
  }

  // 2. Process Live Blinks from Python via USB
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

  // 3. Process physical screen presses from Nextion
  if (Serial2.available() > 0) {
    String incomingCmd = Serial2.readStringUntil('\n');
    incomingCmd.trim();
    if (incomingCmd.length() > 0) {
      interpretBedCommand(incomingCmd);
    }
  }

  // 4. Strict 20ms IMU Sample Loop
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

  // 5. Drive the motor toward active targets
  updateMotorControl();
}