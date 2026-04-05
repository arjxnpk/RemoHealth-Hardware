#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include "MAX30105.h"
#include "heartRate.h"
#include "spo2_algorithm.h"
#include <Adafruit_MPU6050.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include "time.h"
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
// WiFi and Firebase config
const char* ssid = "****-******";
const char* password = "********";
const char* ntpServer = "pool.ntp.org";
const long  gmtOffset_sec = 19800;  // GMT+5:30 for IST (India Standard Time)
const int   daylightOffset_sec = 0;

#define API_KEY "AIzaSyAgyAaY__AdjrQ5hURMaLf_TFg1jOdchSo"
#define PROJECT_ID "healthcareapp-361d0"
#define DATABASE_URL   "https://healthcareapp-361d0-default-rtdb.firebaseio.com/"
const char* firestoreHost = "firestore.googleapis.com";
const int httpsPort = 443;

const char* targetEmail = "stevinsb@gmail.com"; // Change this to your user email

WiFiClientSecure httpsClient;
MAX30105 particleSensor;
Adafruit_MPU6050 mpu;

// Globals for MAX30102
float beatsPerMinute = 0.0;
float spo2 = 0.0;
const byte RATE_SIZE = 4;
byte rates[RATE_SIZE];
byte rateSpot = 0;
long lastBeat = 0;

#define IR_WINDOW_SIZE 10
#define BEAT_THRESHOLD 1000
long irWindow[IR_WINDOW_SIZE] = {0};
int irWindowIndex = 0;
long irMovingAvg = 0;
bool beatDetected = false;

uint32_t irBuffer[100];
uint32_t redBuffer[100];
int32_t bufferLength;
int32_t spo2_calculated;
int8_t validSPO2;
int32_t heartRate;
int8_t validHeartRate;

String userUID = "";
 
// MPU6050 values
sensors_event_t accel, gyro, tempEvent;

// ===== Fall detection globals =====
bool fallDetected = false;
bool possibleFall = false;
unsigned long fallStartMillis = 0;
const float FREEFALL_THRESHOLD = 2.0;    // m/s^2 (near 0g indicates freefall)
const float IMPACT_THRESHOLD = 11.0;     // m/s^2 (~2.5g) for impact detection
const unsigned long FREEFALL_WINDOW = 1000; // 1 second window to catch impact after freefall

// Function declarations
String getUIDfromEmail(const char* email);
float readTemperature();
void readBPMandSpO2();
bool sendToFirestoreREST(float tempC, float tempF, int bpm, float spo2,
                         float ax, float ay, float az, float gx, float gy, float gz);

void setup() {
  Serial.begin(115200);
  Wire.begin();  // ✅ Start I2C bus before initializing sensors
  delay(1000);

  Serial.print("Connecting to WiFi");
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWiFi connected!");

  httpsClient.setInsecure();
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);

  // ✅ Initialize MAX30102
  Serial.println("Initializing MAX30105 sensor...");
  if (!particleSensor.begin(Wire, I2C_SPEED_FAST)) {
    Serial.println("MAX30105 sensor not found! Check wiring.");
    while (1);
  }
  particleSensor.setup(60, 4, 2, 100, 411, 4096);
  particleSensor.enableDIETEMPRDY();

  // ✅ Initialize MPU6050
  Serial.println("Initializing MPU6050...");
  if (!mpu.begin(0x68, &Wire)) {   // explicitly use I2C addr
    Serial.println("Failed to find MPU6050 chip!");
    while (1) delay(10);
  }
  mpu.setAccelerometerRange(MPU6050_RANGE_8_G);
  mpu.setGyroRange(MPU6050_RANGE_500_DEG);
  mpu.setFilterBandwidth(MPU6050_BAND_21_HZ);
  delay(100);
  initOLED();

  // ✅ Fetch UID
  userUID = getUIDfromEmail(targetEmail);
  if (userUID.length() == 0) {
    Serial.println("Failed to fetch UID. Cannot continue.");
    while (1);
  }
  Serial.print("UID fetched: ");
  Serial.println(userUID);
}

void loop() {
  if (userUID.length() == 0) return;

  // MAX30102 readings
  float tempC = readTemperature();
  float tempF = tempC * 1.8 + 32;
  readBPMandSpO2();

  if (tempC < 0 || particleSensor.getIR() < 50000) tempC = 0;
  if (tempF < 0 || particleSensor.getIR() < 50000) tempF = 0;
  if (beatsPerMinute < 0 || particleSensor.getIR() < 50000) beatsPerMinute = 0;
  if (spo2 < 0 || particleSensor.getIR() < 50000) spo2 = 0;

  // MPU6050 readings
  mpu.getEvent(&accel, &gyro, &tempEvent);
  float ax = accel.acceleration.x;
  float ay = accel.acceleration.y;
  float az = accel.acceleration.z;
  float gx = gyro.gyro.x;
  float gy = gyro.gyro.y;
  float gz = gyro.gyro.z;

  // ===== Fall detection algorithm (simple freefall + impact) =====
  // Compute magnitude of acceleration vector
  float accMag = sqrt(ax * ax + ay * ay + az * az);
  Serial.printf("AccMag: %.2f m/s^2\n", accMag);

  // Detect potential freefall (very low accel magnitude)
  if (!possibleFall && accMag > FREEFALL_THRESHOLD) {
    possibleFall = true;
    fallStartMillis = millis();
    // Serial.println("Possible freefall started");
  }

  // If we were in possible freefall, check for impact within window
  if (possibleFall) {
    unsigned long dt = millis() - fallStartMillis;
    if (accMag > IMPACT_THRESHOLD && dt <= FREEFALL_WINDOW) {
      fallDetected = true;
      possibleFall = false; // reset
      Serial.println("Fall detected!");
    } else if (dt > FREEFALL_WINDOW) {
      // timeout - not a fall
      possibleFall = false;
    }
  }

  Serial.printf("Temp: %.2fC, %.2fF | BPM: %.1f | SpO2: %.1f%%\n", tempC, tempF, beatsPerMinute, spo2);
  Serial.printf("Accel X: %.2f Y: %.2f Z: %.2f m/s^2 | Gyro X: %.2f Y: %.2f Z: %.2f rad/s\n", ax, ay, az, gx, gy, gz);
  updateOLED(beatsPerMinute, spo2, tempC);
  if (sendToRTDB(tempC, tempF, (int)beatsPerMinute, spo2, ax, ay, az, gx, gy, gz)) 
  {
    Serial.println("Data sent successfully.");
  } else {
    Serial.println("Failed to send data.");
  }

  // Reset fallDetected after sending once so next event can be detected
  fallDetected = false;

 //delay(3000);
}

// ===== Helper Functions =====

String getUIDfromEmail(const char* email) {

  String url = "/users.json?orderBy=\"email\"&equalTo=\"" + String(email) + "\"";

  if (httpsClient.connected()) {
    httpsClient.stop();
    delay(10);
  }

  if (!httpsClient.connect("healthcareapp-361d0-default-rtdb.firebaseio.com", 443)) {
    Serial.println("RTDB connection failed");
    return "";
  }

  httpsClient.print(String("GET ") + url + " HTTP/1.1\r\n");
  httpsClient.print("Host: healthcareapp-361d0-default-rtdb.firebaseio.com\r\n");
  httpsClient.print("Connection: close\r\n\r\n");

  // Skip HTTP headers
  while (httpsClient.connected()) {
    String line = httpsClient.readStringUntil('\n');
    if (line == "\r") break;
  }

  // Read full response body
  String response = "";
  while (httpsClient.available()) {
    response += (char)httpsClient.read();
  }
  httpsClient.stop();

  Serial.println("RTDB raw response:");
  Serial.println(response);

  // ===== MANUAL UID EXTRACTION (NO JSON PARSING) =====
  int start = response.indexOf("{\"") + 2;
  int end   = response.indexOf("\":{", start);

  if (start > 1 && end > start) {
    String uid = response.substring(start, end);
    Serial.print("UID found: ");
    Serial.println(uid);
    return uid;
  }

  Serial.println("UID not found in RTDB response");
  return "";
}


float readTemperature() {
  return particleSensor.readTemperature();
}

void readBPMandSpO2() {
  long irValue = particleSensor.getIR();

  const int samples = 10;
  long irAvg = 0, redAvg = 0;
  for (int i = 0; i < samples; i++) {
    irAvg += particleSensor.getIR();
    redAvg += particleSensor.getRed();
    delay(1);
  }
  irAvg /= samples;
  redAvg /= samples;

  if (irAvg < 50000) {
    beatsPerMinute = 0.0;
    spo2=0.0;
    for (byte i = 0; i < RATE_SIZE; i++) rates[i] = 0;
    rateSpot = 0;
    lastBeat = 0;
  } else {
    irWindow[irWindowIndex] = irValue;
    irWindowIndex = (irWindowIndex + 1) % IR_WINDOW_SIZE;

    long sumIR = 0;
    for (int i = 0; i < IR_WINDOW_SIZE; i++) sumIR += irWindow[i];
    irMovingAvg = sumIR / IR_WINDOW_SIZE;

    beatDetected = false;
    if (irValue > irMovingAvg + BEAT_THRESHOLD) {
      if (millis() - lastBeat > 300) {
        beatDetected = true;
      }
    }

    if (beatDetected) {
      long delta = millis() - lastBeat;
      lastBeat = millis();

      beatsPerMinute = 60000 / (delta / (delta / 667));
      // No averaging or debug prints here
    }
  }

  bufferLength = 50; // Reduced samples from 100 to 50 for speed
  for (byte i = 0; i < bufferLength; i++) {
    unsigned long startTime = millis();
    while (!particleSensor.available()) {
      particleSensor.check();
      if (millis() - startTime > 5) break; // Timeout 5ms to avoid blocking
    }
    redBuffer[i] = particleSensor.getRed();
    irBuffer[i] = particleSensor.getIR();
    particleSensor.nextSample();
  }
  maxim_heart_rate_and_oxygen_saturation(irBuffer, bufferLength, redBuffer,
                                         &spo2_calculated, &validSPO2, &heartRate, &validHeartRate);
  spo2 = (float)spo2_calculated;
}
String getLocalTimeString() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) {
    return "Time Err";
  }
  char buf[10];
  strftime(buf, sizeof(buf), "%H:%M:%S", &timeinfo);
  return String(buf);
}

 void initOLED() {
  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) { // default I2C addr
    Serial.println("SSD1306 allocation failed");
    for (;;);
  }
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.display();
}
void updateOLED(float bpm, float spo2, float tempC) {
  display.clearDisplay();

  // ✅ Display current time (top center)
  String timeStr = getLocalTimeString();
  display.setTextSize(1);
  int16_t x1, y1;
  uint16_t w, h;
  display.getTextBounds(timeStr, 0, 0, &x1, &y1, &w, &h);
  display.setCursor((SCREEN_WIDTH - w) / 2, 0);
  display.println(timeStr);

  // ✅ Draw grid for BPM and SpO2
  int gridY = 16;
  int gridH = 24;
  int gridW = SCREEN_WIDTH / 2;

  // Left grid (BPM)
  //display.drawRect(0, gridY, gridW, gridH, SSD1306_WHITE);
  display.setCursor(10, gridY + 6);
  display.setTextSize(1);
  display.print("BPM");
  display.setTextSize(2);
  display.setCursor(10, gridY + 14);
  display.print((int)bpm);

  // Right grid (SpO2)
  //display.drawRect(gridW, gridY, gridW, gridH, SSD1306_WHITE);
  display.setCursor(gridW + 10, gridY + 6);
  display.setTextSize(1);
  display.print("SpO2");
  display.setTextSize(2);
  display.setCursor(gridW + 10, gridY + 14);
  display.print((int)spo2);

  // ✅ Temperature below grids
  display.setTextSize(1);
  display.setCursor(10, gridY + gridH + 8);
  display.print("Temp: ");
  display.print(tempC, 1);
  display.print(" C");

  display.display();
}
bool sendToFirestoreREST(float tempC, float tempF, int bpm, float spo2,
                         float ax, float ay, float az, float gx, float gy, float gz) {
  StaticJsonDocument<768> doc;
  JsonObject fields = doc.createNestedObject("fields");
  fields.createNestedObject("temperature_C")["doubleValue"] = tempC;
  fields.createNestedObject("temperature_F")["doubleValue"] = tempF;
  fields.createNestedObject("bpm")["integerValue"] = bpm;
  fields.createNestedObject("spo2")["doubleValue"] = spo2;
  fields.createNestedObject("accel_x")["doubleValue"] = ax;
  fields.createNestedObject("accel_y")["doubleValue"] = ay;
  fields.createNestedObject("accel_z")["doubleValue"] = az;
  fields.createNestedObject("gyro_x")["doubleValue"] = gx;
  fields.createNestedObject("gyro_y")["doubleValue"] = gy;
  fields.createNestedObject("gyro_z")["doubleValue"] = gz;
  fields.createNestedObject("timestamp")["stringValue"] = String(millis());
  // add fall detection boolean
  fields.createNestedObject("fall_detected")["booleanValue"] = fallDetected;

  String body;
  serializeJson(doc, body);

  String url = "/v1/projects/" + String(PROJECT_ID) +
               "/databases/(default)/documents/users/" + userUID +
               "/health_readings?documentId=reading_" + String(millis()) +
               "&key=" + API_KEY;

  if (httpsClient.connected()) httpsClient.stop();
  if (!httpsClient.connect(firestoreHost, httpsPort)) {
    Serial.println("Connection failed");
    return false;
  }

  httpsClient.println("POST " + url + " HTTP/1.1");
  httpsClient.println("Host: firestore.googleapis.com");
  httpsClient.println("Content-Type: application/json");
  httpsClient.print("Content-Length: ");
  httpsClient.println(body.length());
  httpsClient.println();
  httpsClient.print(body);

  while (httpsClient.connected()) {
    String line = httpsClient.readStringUntil('\n');
    if (line == "\r") break;
  }

  String resp = "";
  while (httpsClient.available()) resp += (char)httpsClient.read();
  httpsClient.stop();

  return resp.indexOf("\"name\"") != -1;
}
bool sendToRTDB(float tempC, float tempF, int bpm, float spo2,
                float ax, float ay, float az, float gx, float gy, float gz) {

  StaticJsonDocument<768> doc;
  JsonObject root = doc.to<JsonObject>();
  root["temperature_C"] = tempC;
  root["temperature_F"] = tempF;
  root["bpm"] = bpm;
  root["spo2"] = spo2;
  root["accel_x"] = ax;
  root["accel_y"] = ay;
  root["accel_z"] = az;
  root["gyro_x"] = gx;
  root["gyro_y"] = gy;
  root["gyro_z"] = gz;
  root["fall_detected"] = fallDetected;
  root["timestamp"] = millis();

  String body;
  serializeJson(root, body);

  // NOTE: If your DB is public for testing, omit auth entirely.
  // If you need auth, replace authStr with "?auth=<ID_TOKEN>" (not the API_KEY).
  String authStr = ""; // e.g. "?auth=" + idToken
  String path = "/users/" + userUID + "/health_readings.json" + authStr;

  // ensure any previous connection is closed
  if (httpsClient.connected()) {
    httpsClient.stop();
    delay(10);
  }

  if (!httpsClient.connect("healthcareapp-361d0-default-rtdb.firebaseio.com", 443)) {
    Serial.println("RTDB Connection failed!");
    return false;
  }

  // Send request with Connection: close so server will close socket
  httpsClient.print("POST " + path + " HTTP/1.1\r\n");
  httpsClient.print("Host: healthcareapp-361d0-default-rtdb.firebaseio.com\r\n");
  httpsClient.print("User-Agent: ESP32\r\n");
  httpsClient.print("Content-Type: application/json\r\n");
  httpsClient.print("Connection: close\r\n");
  httpsClient.print("Content-Length: ");
  httpsClient.print(body.length());
  httpsClient.print("\r\n\r\n");
  httpsClient.print(body);

  // Read status line
  String statusLine = httpsClient.readStringUntil('\n');
  statusLine.trim();
  Serial.print("HTTP status line: "); Serial.println(statusLine);

  // parse HTTP status code (e.g. "HTTP/1.1 200 OK")
  int httpCode = 0;
  if (statusLine.length() > 0) {
    int firstSpace = statusLine.indexOf(' ');
    int secondSpace = statusLine.indexOf(' ', firstSpace + 1);
    if (firstSpace > 0 && secondSpace > firstSpace) {
      String codeStr = statusLine.substring(firstSpace + 1, secondSpace);
      httpCode = codeStr.toInt();
    }
  }
  Serial.print("HTTP code: "); Serial.println(httpCode);

  // Skip headers
  while (httpsClient.connected()) {
    String line = httpsClient.readStringUntil('\n');
    if (line == "\r" || line == "") break;
  }

  // Read body
  String resp = "";
  unsigned long start = millis();
  while (httpsClient.available() || (millis() - start) < 200) {
    while (httpsClient.available()) {
      resp += (char)httpsClient.read();
      start = millis();
    }
    delay(1);
  }
  httpsClient.stop();

  Serial.print("RTDB resp: "); Serial.println(resp);

  // Success if 200/201 or if Firebase returned {"name":"-..."} for a push
  if (httpCode == 200 || httpCode == 201 || resp.indexOf("\"name\"") != -1) {
    return true;
  } else {
    Serial.println("RTDB write failed. Check rules/auth/rate limits.");
    return false;
  }
}