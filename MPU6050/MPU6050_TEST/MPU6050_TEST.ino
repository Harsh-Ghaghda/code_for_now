#include <Wire.h>
#include <Arduino.h>

#define MPU_ADDR 0x68  // I2C address of MPU6050

void setup() {
  delay(1000);  // Wait for serial monitor to open
  Serial.begin(9600);     // Higher baud rate for ESP32
  Wire.begin();             // SDA and SCL default to GPIO21 and GPIO22
  delay(1000);

  // Wake up MPU6050
  Wire.beginTransmission(MPU_ADDR);
  Wire.write(0x6B);          // PWR_MGMT_1
  Wire.write(0x00);          // Wake up
  Wire.endTransmission();  // End transmission

  Serial.println("MPU6050 initialized");
}

void loop() {
  Wire.beginTransmission(MPU_ADDR);
  Wire.write(0x3B);          // Starting register for accelerometer
  Wire.endTransmission(false);
  Wire.requestFrom(MPU_ADDR, 6);  // Read accel X/Y/Z

  if (Wire.available() == 6) {
    int16_t ax = Wire.read() << 8 | Wire.read();
    int16_t ay = Wire.read() << 8 | Wire.read();
    int16_t az = Wire.read() << 8 | Wire.read();

    Serial.print("Accel X: "); Serial.print(ax);
    Serial.print(" Y: "); Serial.print(ay);
    Serial.print(" Z: "); Serial.println(az);
  }

  delay(500);
}