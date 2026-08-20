#include <Arduino.h>
#include <AccelStepper.h>
#include <SoftwareSerial.h>
#include <Servo.h>
#include <Wire.h>
#include <Adafruit_MPR121.h>

// --- PIN DEFINITIONS ---
const int stepX = 2;
const int dirX = 5;
const int stepY = 3;
const int dirY = 6;
const int enPin = 8;
const int servoPin = 9; // Your Servo Pin

// --- TCS3200 Pin Definitions ---
const int S0_PIN = 4;
const int S1_PIN = 7;
const int S2_PIN = A0;
const int S3_PIN = A1;
const int sensorOutPin = A2;

// --- MPR121 Sensitivity Registers ---
#define MPR121_CONFIG1 0x5B
#define MPR121_CONFIG2 0x5C

// --- COMMUNICATION ---
// SoftwareSerial for ESP32 (D10=RX, D11=TX)
SoftwareSerial espSerial(10, 11);

// --- HARDWARE INIT ---
AccelStepper stepperX(AccelStepper::DRIVER, stepX, dirX);
AccelStepper stepperY(AccelStepper::DRIVER, stepY, dirY);
Servo liftServo;
Adafruit_MPR121 cap = Adafruit_MPR121();

// --- VARIABLES ---
String espBuffer = "";
bool emergencyHalt = false;
bool targetReachedReported = true;

// Sensor Variables
int baselines[12]; 
int threshold = 4;              
uint16_t lastPattern = 0;      
int matchCount = 0;            
const int REQUIRED_MATCHES = 2; 

unsigned long lastSensorScan = 0;
const unsigned long SENSOR_SCAN_INTERVAL = 50; // Scan every 50ms to not block steppers
unsigned long sensorCooldown = 0;

// --- Helper Function: High Sensitivity Averaging ---
int getAveragedData(int pad, int samples) {
  long total = 0;
  for(int i = 0; i < samples; i++) {
    total += cap.filteredData(pad);
  }
  return total / samples;
}

void setup() {
  // LabVIEW Communication (USB) - Make sure your Serial Monitor is set to 115200!
  Serial.begin(9600);
  
  // ESP32 Communication
  espSerial.begin(9600); 

  // Servo Setup
  liftServo.attach(servoPin);
  
  // CNC Shield Enable
  pinMode(enPin, OUTPUT);
  digitalWrite(enPin, LOW); 

  // Stepper Speeds
  stepperX.setMaxSpeed(1000);
  stepperX.setAcceleration(500);
  stepperY.setMaxSpeed(1000);
  stepperY.setAcceleration(500);

  // 1. Initialize TCS3200 Pins
  pinMode(S0_PIN, OUTPUT);
  pinMode(S1_PIN, OUTPUT);
  pinMode(S2_PIN, OUTPUT);
  pinMode(S3_PIN, OUTPUT);
  pinMode(sensorOutPin, INPUT);
  digitalWrite(S0_PIN, HIGH); // 20% Frequency scaling
  digitalWrite(S1_PIN, LOW);

  // 2. Initialize MPR121
  if (!cap.begin(0x5A)) {
    Serial.println("ERROR: MPR121 not found.");
    // Proceed anyway so steppers still work
  } else {
    // 3. Hardware Sensitivity Boost 
    cap.writeRegister(MPR121_CONFIG1, 0x3F); 
    cap.writeRegister(MPR121_CONFIG2, 0x20); 

    // 4. Silent Calibration
    Serial.println("Calibrating touch sensor...");
    for (int i = 0; i < 12; i++) {
      baselines[i] = getAveragedData(i, 10);
    }
  }

  Serial.println("SYSTEM_READY");
}

void loop() {
  // =========================================================
  // TASK 1: RECEIVE COMMAND FROM LABVIEW (X,Y,Servo)
  // =========================================================
  if (Serial.available() > 0) {
    String input = Serial.readStringUntil('\n');
    input.trim();

    int firstComma = input.indexOf(',');
    int secondComma = input.lastIndexOf(',');

    if (firstComma != -1 && secondComma != -1) {
      long targetX = input.substring(0, firstComma).toInt();
      long targetY = input.substring(firstComma + 1, secondComma).toInt();
      int servoPos = input.substring(secondComma + 1).toInt();

      // Execute Movements
      stepperX.moveTo(targetX);
      stepperY.moveTo(targetY);
      liftServo.write(servoPos);

      // Reset states
      emergencyHalt = false;
      targetReachedReported = false;

      // 1. Echo to LabVIEW for confirmation
      Serial.print("MOVING_TO: ");
      Serial.print(targetX); Serial.print(","); 
      Serial.print(targetY); Serial.print(","); 
      Serial.println(servoPos);

      // 2. ONLY SEND TO ESP32 WHEN LABVIEW ISSUES COMMAND
      espSerial.print("POS:");
      espSerial.print(stepperX.currentPosition());
      espSerial.print(",");
      espSerial.print(stepperY.currentPosition());
      espSerial.println(",moving");
    }
  }

  // =========================================================
  // TASK 2: RECEIVE EMERGENCY COMMANDS FROM ESP32 (Web/AI)
  // =========================================================
  while (espSerial.available() > 0) {
    char c = espSerial.read();
    if (c == '\n') {
      espBuffer.trim();

      Serial.print("ESP32_RECEIVED: ");
      Serial.println(espBuffer); // Debug print what we actually get

      if (espBuffer == "HALT") {
        stepperX.stop();
        stepperY.stop();
        emergencyHalt = true;
        Serial.println("WARNING: EMERGENCY_HALT_FROM_WEB");
        
        // Tell ESP32 we successfully halted
        espSerial.print("POS:");
        espSerial.print(stepperX.currentPosition());
        espSerial.print(",");
        espSerial.print(stepperY.currentPosition());
        espSerial.println(",HALTED");
      }
      else if (espBuffer.startsWith("X") && espBuffer.indexOf('Y') > 0) {
        // Parse "X400,Y900"
        int commaIndex = espBuffer.indexOf(',');
        if (commaIndex > 0) {
          long targetX = espBuffer.substring(1, commaIndex).toInt();
          long targetY = espBuffer.substring(commaIndex + 2).toInt(); // Skip ",Y"
          
          stepperX.moveTo(targetX);
          stepperY.moveTo(targetY);
          
          emergencyHalt = false;
          targetReachedReported = false;
          
          Serial.print("WEB_DISPATCH_TO: ");
          Serial.print(targetX); Serial.print(","); Serial.println(targetY);
          
          espSerial.print("POS:");
          espSerial.print(stepperX.currentPosition());
          espSerial.print(",");
          espSerial.print(stepperY.currentPosition());
          espSerial.println(",moving");
        }
      }
      
      espBuffer = ""; // Clear for next message
    } else if (c != '\r') {
      espBuffer += c;
    }
  }

  // =========================================================
  // TASK 3: SENSOR POLLING (Non-Blocking)
  // =========================================================
  if (millis() - lastSensorScan >= SENSOR_SCAN_INTERVAL && millis() > sensorCooldown) {
    lastSensorScan = millis();
    uint16_t currentPattern = 0;

    for (int i = 0; i < 12; i++) {
      int currentData = getAveragedData(i, 3); 
      int delta = baselines[i] - currentData;
      if (delta > threshold) {
        currentPattern |= (1 << i); 
      } 
    }

    if (currentPattern == 0) {
      matchCount = 0;
      lastPattern = 0;
    } 
    else if (currentPattern == lastPattern) {
      matchCount++;
      if (matchCount >= REQUIRED_MATCHES) {
        
        // Perform Color Reading
        int r, g, b;
        
        // Using a 10000us (10ms) timeout so pulseIn doesn't freeze the steppers!
        digitalWrite(S2_PIN, LOW); digitalWrite(S3_PIN, LOW);
        r = pulseIn(sensorOutPin, LOW, 10000);
        
        digitalWrite(S2_PIN, HIGH); digitalWrite(S3_PIN, HIGH);
        g = pulseIn(sensorOutPin, LOW, 10000);
        
        digitalWrite(S2_PIN, LOW); digitalWrite(S3_PIN, HIGH);
        b = pulseIn(sensorOutPin, LOW, 10000);
        
        String cardColor = "UNKNOWN";
        
        // Verification Logic
        if (r > 0 && g > 0 && b > 0) { // Check that it didn't timeout
          if (r < g && r < b) {
            cardColor = "RED";
          } else if (g < r && g < b) {
            cardColor = "GREEN";
          } else if (b < r && b < g) {
            cardColor = "BLUE";
          }
        }

        // Send to LabVIEW with a prefix
        Serial.print("SENSOR_DATA: ");
        Serial.print(currentPattern);
        Serial.print(" ");
        Serial.println(cardColor);

        // Reset for next swipe and apply cooldown
        matchCount = 0;
        lastPattern = 0;
        sensorCooldown = millis() + 1000; // 1 second cooldown (replaces delay(1000))
      }
    } 
    else {
      matchCount = 0;
      lastPattern = currentPattern;
    }
  }

  // =========================================================
  // TASK 4: MOVEMENT EXECUTION & LIVE TELEMETRY
  // =========================================================
  if (!emergencyHalt) {
    stepperX.run();
    stepperY.run();

    // Broadcast actual physical position to the Web Dashboard every 250ms!
    static unsigned long lastTelemetry = 0;
    if ((stepperX.distanceToGo() != 0 || stepperY.distanceToGo() != 0) && millis() - lastTelemetry > 250) {
        lastTelemetry = millis();
        espSerial.print("POS:");
        espSerial.print(stepperX.currentPosition());
        espSerial.print(",");
        espSerial.print(stepperY.currentPosition());
        espSerial.println(",moving");
    }
  }

  // =========================================================
  // TASK 5: LABVIEW TARGET REACHED
  // =========================================================
  if (!stepperX.isRunning() && !stepperY.isRunning()) {
    if (!targetReachedReported) {
      Serial.println("TARGET_REACHED");
      
      // Tell ESP32 we arrived and are idle
      espSerial.print("POS:");
      espSerial.print(stepperX.currentPosition());
      espSerial.print(",");
      espSerial.print(stepperY.currentPosition());
      espSerial.println(",idle");
      
      targetReachedReported = true;
    }
  }
}