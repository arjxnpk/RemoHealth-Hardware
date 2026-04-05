#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include "time.h"

// ================= WIFI & FIREBASE =================
const char* ssid = "****-******";
const char* password = "*********";

#define API_KEY "AIzaSyAgyAaY__AdjrQ5hURMaLf_TFg1jOdchSo"
#define PROJECT_ID "healthcareapp-361d0"
#define DATABASE_URL "https://healthcareapp-361d0-default-rtdb.firebaseio.com/"

const char* firestoreHost = "firestore.googleapis.com";
const int httpsPort = 443;

// Email whose UID is used
const char* targetEmail = "stevinsb@gmail.com";

// Time (IST)
const char* ntpServer = "pool.ntp.org";
const long gmtOffset_sec = 19800;
const int daylightOffset_sec = 0;

WiFiClientSecure httpsClient;
String userUID = "";

// ================= AD8232 ECG =================
#define ECG_PIN 36
#define LO_PLUS 23
#define LO_MINUS 22

#define ECG_BUFFER_SIZE 50
int ecgBuffer[ECG_BUFFER_SIZE];
int ecgIndex = 0;

// ================= FUNCTION DECLARATIONS =================
String getUIDfromEmailRTDB(const char* email);
bool sendECGtoRTDB(int ecgRaw, JsonArray ecgSamples);

// ================= SETUP =================
void setup() {
  Serial.begin(115200);
  delay(1000);

  pinMode(LO_PLUS, INPUT);
  pinMode(LO_MINUS, INPUT);

  analogReadResolution(12);
  analogSetAttenuation(ADC_11db);

  Serial.println("Connecting to WiFi...");
  WiFi.begin(ssid, password);

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  Serial.println("\nWiFi connected");

  httpsClient.setInsecure();
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);

  userUID = getUIDfromEmailRTDB(targetEmail);

  if (userUID.length() == 0) {
    // userUID = "KNC4mIXcZ0Vp3t8SvhUk841FOvF2";
    Serial.println("UID fetch failed. Halting.");
    while (1);
  }

  Serial.print("Uploading ECG data for UID: ");
  Serial.println(userUID);
}

// ================= LOOP =================
void loop() {
  if (digitalRead(LO_PLUS) == HIGH || digitalRead(LO_MINUS) == HIGH) {
    Serial.println("ECG Leads OFF");
    delay(500);
    return;
  }

  int ecgRaw = analogRead(ECG_PIN);
  ecgBuffer[ecgIndex++] = ecgRaw;

  if (ecgIndex >= ECG_BUFFER_SIZE)
    ecgIndex = 0;

  StaticJsonDocument<1024> doc;
  JsonArray ecgSamples = doc.createNestedArray("samples");

  for (int i = 0; i < ECG_BUFFER_SIZE; i++) {
    ecgSamples.add(ecgBuffer[i]);
  }

  Serial.println(ecgRaw);
  sendECGtoRTDB(ecgRaw, ecgSamples);

  /*
  if (sendECGtoRTDB(ecgRaw, ecgSamples)) {
    Serial.println("ECG data uploaded");
  } else {
    Serial.println("ECG upload failed");
  }
  */

  delay(50);
}

// ================= UID FETCH =================
String getUIDfromEmailRTDB(const char* email) {
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


// ================= RTDB UPLOAD =================
bool sendECGtoRTDB(int ecgRaw, JsonArray ecgSamples) {
  StaticJsonDocument<1024> doc;
  JsonObject root = doc.to<JsonObject>();

  root["ecg_raw"] = ecgRaw;
  // root["ecg_samples"] = ecgSamples;
  root["timestamp"] = millis();

  String body;
  serializeJson(root, body);

  String path = "/users/" + userUID + "/ecg_readings.json";

  if (httpsClient.connected())
    httpsClient.stop();

  if (!httpsClient.connect("healthcareapp-361d0-default-rtdb.firebaseio.com", 443)) {
    return false;
  }

  httpsClient.print("POST " + path + " HTTP/1.1\r\n");
  httpsClient.print("Host: healthcareapp-361d0-default-rtdb.firebaseio.com\r\n");
  httpsClient.print("Content-Type: application/json\r\n");
  httpsClient.print("Connection: close\r\n");
  httpsClient.print("Content-Length: ");
  httpsClient.print(body.length());
  httpsClient.print("\r\n\r\n");
  httpsClient.print(body);

  while (httpsClient.connected()) {
    if (httpsClient.readStringUntil('\n') == "\r")
      break;
  }

  String resp = httpsClient.readString();
  httpsClient.stop();

  return resp.indexOf("\"name\"") != -1;
}