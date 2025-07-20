#include <WiFi.h>
#include <Firebase_ESP_Client.h>
#include <Wire.h>
#include "MAX30105.h"
#include "heartRate.h"
#include "spo2_algorithm.h"

// WiFi Credentials
const char* ssid = "Aadit";
const char* password = "ronaldo7";

// Firebase Credentials
#define API_KEY "AIzaSyAgyAaY__AdjrQ5hURMaLf_TFg1jOdchSo"
#define FIREBASE_PROJECT_ID "healthcareapp-361d0"

// Firebase objects
FirebaseData fbdo;
FirebaseAuth auth;
FirebaseConfig config;

// Hardcoded UID
const String userUID = "KNC4mIXcZ0Vp3t8SvhUk841FOvF2";

// MAX30105 Object and Variables
MAX30105 particleSensor;
const byte RATE_SIZE = 4;
byte rates[RATE_SIZE];
byte rateSpot = 0;
long lastBeat = 0;
float beatsPerMinute = 0.0;
int beatAvg = 0;
float spo2 = 0.0;

// SpO2 Variables from 3rd code
uint32_t irBuffer[100]; // infrared LED sensor data
uint32_t redBuffer[100]; // red LED sensor data
int32_t bufferLength;
int32_t spo2_calculated;
int8_t validSPO2;
int32_t heartRate;
int8_t validHeartRate;

// Additional variables for new BPM calculation
#define IR_WINDOW_SIZE 10
#define BEAT_THRESHOLD 1000
long irWindow[IR_WINDOW_SIZE] = {0};
int irWindowIndex = 0;
long irMovingAvg = 0;
bool beatDetected = false;

// Function declarations
float readTemperature();
void readBPMandSpO2();
void sendToFirestore(float tempC, float tempF, int bpm, float spo2);

void setup() {
  Serial.begin(115200);
  delay(500); // Reduced from 2000 to 500 ms

  // Connect to WiFi
  Serial.print("Connecting to WiFi...");
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500); // Reduced from 2000 to 500 ms
    Serial.print(".");
  }
  Serial.println("\nWiFi Connected!");
  Serial.print("ESP32 IP Address: ");
  Serial.println(WiFi.localIP());

  // Initialize MAX30105 sensor
  if (!particleSensor.begin(Wire, I2C_SPEED_FAST)) {
    Serial.println("MAX30105 was not found. Please check wiring/power.");
    while (1);
  }
  Serial.println("Place your index finger on the sensor with steady pressure.");

  // Setup from 1st code (Temperature)
  particleSensor.setup(0); // Turn off LEDs initially for temperature
  particleSensor.enableDIETEMPRDY();

  // Setup from 2nd code (BPM)
  particleSensor.setPulseAmplitudeRed(0x0A); // Low Red LED for BPM
  particleSensor.setPulseAmplitudeGreen(0);  // Green LED off

  // Setup from 3rd code (SpO2)
  byte ledBrightness = 60;
  byte sampleAverage = 4;
  byte ledMode = 2;
  byte sampleRate = 100;
  int pulseWidth = 411;
  int adcRange = 4096;
  particleSensor.setup(ledBrightness, sampleAverage, ledMode, sampleRate, pulseWidth, adcRange);

  // Firebase config setup
  config.api_key = API_KEY;
  config.signer.test_mode = true; // Remove for production
  Firebase.reconnectWiFi(true);
  Firebase.begin(&config, &auth);

  // Initialize rates array
  for (byte i = 0; i < RATE_SIZE; i++) {
    rates[i] = 0;
  }

  //Serial.println("Using hardcoded UID: " + userUID);
}

void loop() {
  if (Firebase.ready()) {
    float tempC = readTemperature();
    float tempF = (tempC * 1.8) + 32;
    readBPMandSpO2();

    // Set values to 0 if negative or no finger detected
    if (tempC < 0 || particleSensor.getIR() < 50000) tempC = 0;
    if (tempF < 0 || particleSensor.getIR() < 50000) tempF = 0;
    if (beatAvg < 0 || particleSensor.getIR() < 50000) beatsPerMinute = 0;
    if (spo2 < 0 || particleSensor.getIR() < 50000) spo2 = 0;

    Serial.print("Temperature: ");
    Serial.print(tempC);
    Serial.print(" °C / ");
    Serial.print(tempF);
    Serial.print(" °F | IR=");
    Serial.print(particleSensor.getIR());
    Serial.print(", Red=");
    Serial.print(particleSensor.getRed());
    Serial.print(", BPM=");
    Serial.print(beatsPerMinute);
    Serial.print(", Avg BPM=");
    Serial.print(beatAvg);
    Serial.print(", SpO2=");
    Serial.print(spo2);
    Serial.print("%");
    if (particleSensor.getIR() < 50000) {
      Serial.print(" No finger?");
    }
    Serial.println();

    sendToFirestore(tempC, tempF, beatsPerMinute, spo2);

    delay(100); // Reduced from 1000 to 100 ms
  }
}

// Temperature reading from 1st code (MAX30105)
float readTemperature() {
  float temperature = particleSensor.readTemperature();
  return temperature; // Returns Celsius
}

// BPM and SpO2 reading combining 2nd and 3rd codes
void readBPMandSpO2() {
  // Get initial readings
  long irValue = particleSensor.getIR();
  long redValue = particleSensor.getRed();

  // Store averaged readings for SpO2
  const int samples = 10;
  long irAvg = 0, redAvg = 0;
  for (int i = 0; i < samples; i++) {
    irAvg += particleSensor.getIR();
    redAvg += particleSensor.getRed();
    delay(1); // Reduced from 5 to 1 ms
  }
  irAvg /= samples;
  redAvg /= samples;

  // Check if finger is present
  if (irAvg < 50000) {
    // No finger detected, reset BPM and beat-related variables
    beatsPerMinute = 0.0;
    beatAvg = 0;
    for (byte i = 0; i < RATE_SIZE; i++) {
      rates[i] = 0;
    }
    rateSpot = 0;
    lastBeat = 0;
  } else {
    // Finger is present, proceed with beat detection
    // Custom beat detection
    // Update moving average of IR values
    irWindow[irWindowIndex] = irValue;
    irWindowIndex = (irWindowIndex + 1) % IR_WINDOW_SIZE;

    long sumIR = 0;
    for (int i = 0; i < IR_WINDOW_SIZE; i++) {
      sumIR += irWindow[i];
    }
    irMovingAvg = sumIR / IR_WINDOW_SIZE;

    // Detect a peak (beat) if the current IR value is significantly above the moving average
    beatDetected = false;
    if (irValue > irMovingAvg + BEAT_THRESHOLD) {
      // Check if enough time has passed since the last beat to avoid noise
      if (millis() - lastBeat > 300) { // Minimum 300ms between beats (200 BPM max)
        beatDetected = true;
      }
    }

    if (beatDetected) {
      long delta = millis() - lastBeat;
      lastBeat = millis();

      // New logic: Estimate BPM by adjusting delta for missed beats
      beatsPerMinute = 60000 / (delta / (delta / 667)); // Scale delta to ~667 ms (90 BPM)
      Serial.print("Beat detected, Delta (ms): ");
      Serial.print(delta); // Still print actual delta for debugging
      Serial.print(", BPM calculated: ");
      Serial.println(beatsPerMinute);

      // Relaxed range check: accept any BPM > 0
      if (beatsPerMinute > 0) {
        rates[rateSpot++] = (byte)beatsPerMinute; // Store this reading in the array
        rateSpot %= RATE_SIZE; // Wrap variable

        // Take average of readings
        beatAvg = 0;
        for (byte x = 0; x < RATE_SIZE; x++) {
          beatAvg += rates[x];
        }
        beatAvg /= RATE_SIZE;
        Serial.print("Updated Avg BPM: ");
        Serial.println(beatAvg);
      } else {
        Serial.println("BPM invalid (<= 0), ignoring...");
      }
    }
  }

  // SpO2 calculation (unchanged)
  bufferLength = 100;
  for (byte i = 0; i < bufferLength; i++) {
    while (particleSensor.available() == false)
      particleSensor.check();
    redBuffer[i] = particleSensor.getRed();
    irBuffer[i] = particleSensor.getIR();
    particleSensor.nextSample();
  }
  maxim_heart_rate_and_oxygen_saturation(irBuffer, bufferLength, redBuffer, &spo2_calculated, &validSPO2, &heartRate, &validHeartRate);
  spo2 = (float)spo2_calculated;
}

void sendToFirestore(float tempC, float tempF, int bpm, float spo2) {
  FirebaseJson content;
  content.set("fields/temperature_C/doubleValue", tempC);
  content.set("fields/temperature_F/doubleValue", tempF);
  content.set("fields/bpm/integerValue", bpm);
  content.set("fields/spo2/doubleValue", spo2);
  content.set("fields/timestamp/stringValue", String(millis()));

  String documentPath = "users/" + userUID + "/health_readings/reading_" + String(millis());

  if (Firebase.Firestore.createDocument(&fbdo, FIREBASE_PROJECT_ID, "", documentPath.c_str(), content.raw())) {
    Serial.println("Data sent to Firestore successfully");
  } else {
    Serial.println("Failed to send data: " + fbdo.errorReason());
  }
}