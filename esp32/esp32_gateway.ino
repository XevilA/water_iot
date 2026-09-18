/**
 * Project: Smart Auto-Watering & LINE Messaging API Gateway
 * Device: ESP32 IoT Gateway (NodeMCU-32S)
 * Features:
 *   - Modular Configuration via config.h
 *   - Vercel Web API integration (POST https://apiline.vercel.app/api/farm)
 *   - LINE Messaging API (Push Message to Group / User)
 *   - Firebase Realtime Database cloud sync
 *   - Non-blocking millis() multitasking loop
 *   - Ultrasonic HC-SR04 dry-run protection (< 30cm safety)
 *   - Bidirectional UART communication with micro:bit V2
 */

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HardwareSerial.h>
#include <HTTPClient.h>
#include "config.h"

// ================= 📌 กำหนดขาเชื่อมต่อฮาร์ดแวร์ =================
#define PIN_TRIG            25    // ขา Trig ของ HC-SR04
#define PIN_ECHO            26    // ขา Echo ของ HC-SR04 (ผ่านวงจรแบ่งแรงดัน 5V -> 3.3V)
#define RXD2                16    // ต่อรับสัญญาณจาก micro:bit P8 (TX)
#define TXD2                17    // ต่อส่งสัญญาณไปยัง micro:bit P14 (RX)

// ================= ⏱️ ตัวแปรการจัดตารางเวลา (Non-blocking Timers) =================
unsigned long lastSensorCheck   = 0;
unsigned long lastApiSync       = 0;
unsigned long lastFirebaseSync  = 0;

// ตัวแปรจับเวลาแยกสำหรับแต่ละสถานะของ LINE Alert (Cooldown 60s)
unsigned long lastLineStart     = 0;
unsigned long lastLineDone      = 0;
unsigned long lastLineLowWater  = 0;
unsigned long lastLineError     = 0;

// ตัวแปรสถานะระบบ
int currentSoilValue    = 0;
int currentPumpNumeric  = 0;      // 0 = OFF, 1 = ON
String currentPumpState = "IDLE"; // "IDLE", "RUNNING", "ERROR"
float currentWaterDist  = 0.0;
bool isWaterLow         = false;

// อินสแตนซ์ Serial 2 สื่อสารกับ micro:bit
HardwareSerial microbitSerial(2);

// ================= 📡 ฟังก์ชันวัดระยะทางด้วยคลื่นอัลตราโซนิก =================
float readUltrasonicDistance() {
  digitalWrite(PIN_TRIG, LOW);
  delayMicroseconds(2);
  digitalWrite(PIN_TRIG, HIGH);
  delayMicroseconds(10);
  digitalWrite(PIN_TRIG, LOW);

  // วัดเวลาพัลส์สะท้อนกลับ (Timeout 30,000us)
  long duration = pulseIn(PIN_ECHO, HIGH, 30000);
  if (duration == 0) {
    return INVALID_DISTANCE;
  }
  return (duration * 0.0343) / 2.0;
}

// ================= 💬 ฟังก์ชันส่ง LINE Push Message (Messaging API) =================
void sendLinePushMessage(String messageText) {
  if (!LINE_ENABLED || WiFi.status() != WL_CONNECTED) return;

  WiFiClientSecure client;
  client.setInsecure(); // ละเว้นการตรวจ CA certificate เพื่อประสิทธิภาพสูงสุด

  HTTPClient https;
  https.begin(client, "https://api.line.me/v2/bot/message/push");
  https.addHeader("Content-Type", "application/json");
  https.addHeader("Authorization", "Bearer " + String(LINE_TOKEN));

  // ส่งไปยัง LINE_GROUP_ID ตามที่คอนฟิกไว้
  String jsonBody = "{";
  jsonBody += "\"to\":\"" + String(LINE_GROUP_ID) + "\",";
  jsonBody += "\"messages\":[";
  jsonBody += "{\"type\":\"text\",\"text\":\"" + messageText + "\"}";
  jsonBody += "]";
  jsonBody += "}";

  int httpResponseCode = https.POST(jsonBody);
  if (httpResponseCode == 200) {
    Serial.println("[LINE Push] ส่งสำเร็จ -> " + messageText);
  } else {
    Serial.printf("[LINE Push] เกิดข้อผิดพลาด Code: %d\n", httpResponseCode);
    Serial.println("[LINE Push Response]: " + https.getString());
  }
  https.end();
}

// ================= 🌐 ฟังก์ชันส่งข้อมูลเข้า Web API บน Vercel =================
void postToVercelApi(int soil, float waterDist, int pumpVal) {
  if (WiFi.status() != WL_CONNECTED) return;

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient https;

  https.begin(client, API_URL);
  https.addHeader("Content-Type", "application/json");
  https.addHeader("x-api-key", API_KEY);

  // Payload ส่งไปยัง apiline.vercel.app/api/farm
  String jsonBody = "{";
  jsonBody += "\"soil\":" + String(soil) + ",";
  jsonBody += "\"distance\":" + String(waterDist, 1) + ",";
  if (API_PUMP_AS_BOOLEAN) {
    jsonBody += "\"pump\":" + String(pumpVal == 1 ? "true" : "false");
  } else {
    jsonBody += "\"pump\":" + String(pumpVal);
  }
  jsonBody += "}";

  int httpCode = https.POST(jsonBody);
  if (httpCode > 0) {
    Serial.printf("[Vercel API] POST สำเร็จ Code: %d\n", httpCode);
  } else {
    Serial.printf("[Vercel API] ล้มเหลว Code: %d\n", httpCode);
  }
  https.end();
}

// ================= ☁️ ฟังก์ชันอัปโหลดสถานะขึ้น Firebase Realtime Database =================
void syncToFirebase(int soil, float waterDist, String pumpState) {
  if (WiFi.status() != WL_CONNECTED) return;

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient https;

  String url = String(FIREBASE_HOST) + "/telemetry.json?auth=" + String(FIREBASE_AUTH);
  https.begin(client, url);
  https.addHeader("Content-Type", "application/json");

  // สร้างเพย์โหลด JSON ที่ถูกต้องตาม Security Rules
  String jsonPayload = "{";
  jsonPayload += "\"soil_moisture\":" + String(soil) + ",";
  jsonPayload += "\"water_distance_cm\":" + String(waterDist, 1) + ",";
  jsonPayload += "\"pump_state\":\"" + pumpState + "\",";
  jsonPayload += "\"timestamp\":{\".sv\":\"timestamp\"}";
  jsonPayload += "}";

  int httpCode = https.PUT(jsonPayload);
  if (httpCode == HTTP_CODE_OK) {
    Serial.println("[Firebase] ซิงค์ข้อมูลสำเร็จ");
  }
  https.end();
}

// ================= 🚀 Setup เริ่มต้นระบบ =================
void setup() {
  Serial.begin(115200);
  microbitSerial.begin(9600, SERIAL_8N1, RXD2, TXD2);

  pinMode(PIN_TRIG, OUTPUT);
  pinMode(PIN_ECHO, INPUT);

  Serial.println("\n--- เริ่มต้นระบบ Smart Auto-Watering IoT Gateway ---");
  Serial.printf("Connecting to Wi-Fi SSID: %s\n", WIFI_SSID);

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  int retries = 0;
  while (WiFi.status() != WL_CONNECTED && retries < 25) {
    delay(500);
    Serial.print(".");
    retries++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("\n[Wi-Fi] เชื่อมต่อสำเร็จ! IP: %s\n", WiFi.localIP().toString().c_str());
  } else {
    Serial.println("\n[Wi-Fi] เชื่อมต่อล้มเหลว ตรวจสอบ SSID/Password");
  }
}

// ================= 🔄 Loop ประมวลผลหลัก (Non-blocking) =================
void loop() {
  unsigned long now = millis();

  // 1. ตรวจสอบข้อมูล UART ที่ส่งมาจาก micro:bit V2 ตลอดเวลา
  if (microbitSerial.available()) {
    String rawMsg = microbitSerial.readStringUntil('\n');
    rawMsg.trim();

    if (rawMsg.length() > 0) {
      Serial.println("[UART In]: " + rawMsg);

      // ตรวจสอบสัญญาณแจ้งเตือนเหตุการณ์
      if (rawMsg.startsWith("ALERT:START")) {
        currentPumpNumeric = 1;
        currentPumpState = "RUNNING";
        if (now - lastLineStart >= LINE_COOLDOWN_MS) {
          sendLinePushMessage("🟢 ดินแห้ง เริ่มรดน้ำอัตโนมัติ (ความชื้นดิน: " + String(currentSoilValue) + ")");
          lastLineStart = now;
        }
      }
      else if (rawMsg.startsWith("ALERT:DONE")) {
        currentPumpNumeric = 0;
        currentPumpState = "IDLE";
        if (now - lastLineDone >= LINE_COOLDOWN_MS) {
          sendLinePushMessage("🔵 รดน้ำเสร็จแล้ว (ความชื้นดิน: " + String(currentSoilValue) + ")");
          lastLineDone = now;
        }
      }
      else if (rawMsg.startsWith("ALERT:ERROR_TIMEOUT")) {
        currentPumpNumeric = 0;
        currentPumpState = "ERROR";
        if (now - lastLineError >= LINE_COOLDOWN_MS) {
          sendLinePushMessage("⚠️ ระบบผิดปกติ: ปั๊มน้ำทำงานเกิน 60 วินาที ตัดระบบฉุกเฉิน โปรดตรวจสอบแปลงเกษตร");
          lastLineError = now;
        }
      }
      else if (rawMsg.startsWith("DATA:")) {
        // แกะข้อมูล DATA:PUMP_ON:860 หรือ DATA:IDLE:640
        int firstColon = rawMsg.indexOf(':');
        int secondColon = rawMsg.indexOf(':', firstColon + 1);
        if (secondColon != -1) {
          currentPumpState = rawMsg.substring(firstColon + 1, secondColon);
          currentPumpNumeric = (currentPumpState == "PUMP_ON" || currentPumpState == "RUNNING") ? 1 : 0;
          currentSoilValue = rawMsg.substring(secondColon + 1).toInt();
        }
      }
    }
  }

  // 2. อ่านระยะผิวน้ำทุกๆ DIST_SEND_MS (500ms)
  if (now - lastSensorCheck >= DIST_SEND_MS) {
    lastSensorCheck = now;
    float dist = readUltrasonicDistance();

    if (dist != INVALID_DISTANCE) {
      currentWaterDist = dist;

      // ตรวจสอบเงื่อนไขระดับน้ำต่ำ (Dry-run Protection)
      if (currentWaterDist >= TANK_EMPTY_DIST_CM) {
        if (!isWaterLow) {
          isWaterLow = true;
          // สั่งหยุดปั๊มน้ำฉุกเฉินไปยัง micro:bit
          microbitSerial.println("CMD:STOP");

          if (now - lastLineLowWater >= LINE_COOLDOWN_MS) {
            sendLinePushMessage("🔴 น้ำในถังใกล้หมด กรุณาเติมน้ำ (ระยะห่างผิวน้ำ: " + String(currentWaterDist, 1) + " ซม.)");
            lastLineLowWater = now;
          }
        }
      } else {
        if (isWaterLow) {
          isWaterLow = false;
          microbitSerial.println("CMD:RESUME");
        }
      }
    }
  }

  // 3. ซิงค์ขึ้น Web API บน Vercel ทุกๆ API_INTERVAL_MS (5 วินาที)
  if (now - lastApiSync >= API_INTERVAL_MS) {
    lastApiSync = now;
    postToVercelApi(currentSoilValue, currentWaterDist, currentPumpNumeric);
  }

  // 4. ซิงค์ขึ้น Firebase Realtime Database ทุก 30 วินาที
  if (now - lastFirebaseSync >= 30000) {
    lastFirebaseSync = now;
    syncToFirebase(currentSoilValue, currentWaterDist, currentPumpState);
  }
}
