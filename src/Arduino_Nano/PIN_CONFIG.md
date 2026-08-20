# Arduino Nano Pin Configuration

## Stepper Motors (CNC Shield)
* **D2**: Stepper X Step
* **D5**: Stepper X Direction
* **D3**: Stepper Y Step
* **D6**: Stepper Y Direction
* **D8**: Stepper Enable (Active Low)

## Actuation
* **D9**: Lift Servo

## Communication
* **D10**: RX for ESP32 Communication (SoftwareSerial)
* **D11**: TX for ESP32 Communication (SoftwareSerial)

## Sensors
### TCS3200 Color Sensor
* **D4**: S0 (Frequency Scaling)
* **D7**: S1 (Frequency Scaling)
* **A0** (D14): S2 (Color Filter)
* **A1** (D15): S3 (Color Filter)
* **A2** (D16): sensorOut (Frequency Output)

### MPR121 Capacitive Touch Sensor
* **A4**: SDA (I2C)
* **A5**: SCL (I2C)
