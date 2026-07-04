#include <Arduino.h>

#define ENCODER_PIN A0

int rotations = 0;
float lastAngle = 0;

void setup() {
    Serial.begin(9600);
    Serial.println("AS5600 Analog Rotation Counter");
}

void loop() {
    int raw = analogRead(ENCODER_PIN);

    float angle = (raw / 1023.0) * 360.0;

    float diff = angle - lastAngle;

    if (diff < -200.0) {
        rotations++;
    }

    if (diff > 200.0) {
        rotations--;
    }

    Serial.print("Angle: ");
    Serial.print(angle);

    Serial.print(" | Rotations: ");
    Serial.println(rotations);
    lastAngle = angle;

    delay(5);
}