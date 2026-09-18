/**
 * Project: Smart Auto-Watering & LINE Messaging API Gateway
 * Device: ESP32 IoT Gateway (NodeMCU-32S)
 * Features:
 *   - LINE Messaging API (Push Message) via Channel Access Token
 *   - Firebase Realtime Database cloud sync (30s interval)
 *   - Non-blocking millis() multitasking loop
 *   - LINE Rate-limiting cooldown (60s / state)
 *   - Ultrasonic HC-SR04 dry-run protection (< 30cm safety)
 *   - Bidirectional UART communication with micro:bit V2
 */

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HardwareSerial.h>
#include <HTTPClient.h>

// ================= ⚙️ การตั้งค่าระบบเครือข่ายและ Cloud =================
const char* WIFI_SSID       = "YOUR_WIFI_SSID";
const char* WIFI_PASSWORD   = "YOUR_WIFI_PASSWORD";

// ---------------- 💬 LINE Messaging API (แบบใหม่ Official) ----------------
// ได้จาก LINE Developers Console > Messaging API > Channel access token (long-lived)
const char* LINE_CHANNEL_ACCESS_TOKEN = "YOUR_LINE_CHANNEL_ACCESS_TOKEN";

// Channel Secret (สำหรับตรวจสอบความถูกต้องกรณีทำ Webhook รับคำสั่ง)
const char* LINE_CHANNEL_SECRET       = "YOUR_LINE_CHANNEL_SECRET";

// User ID หรือ Group ID ที่ต้องการให้ Bot ยิงข้อความแจ้งเตือนไปหา (ได้จากหน้า Messaging API หรือ Webhook event)
const char* LINE_USER_ID              = "YOUR_LINE_USER_OR_GROUP_ID";

// ---------------- ☁️ Firebase Realtime Database ----------------
const char* FIREBASE_HOST   = "https://water-iot-prod-2026-default-rtdb.asia-southeast1.firebasedatabase.app";
const char* FIREBASE_AUTH   = "YOUR_DATABASE_SECRET_OR_WEB_API_KEY";

// ================= 📌 กำหนดขาเชื่อมต่อฮาร์ดแวร์ =================
#define PIN_TRIG            25    // ขา Trig ของ HC-SR04
#define PIN_ECHO            26    // ขา Echo ของ HC-SR04 (ผ่านวงจรแบ่งแรงดัน 5V -> 3.3V)
#define RXD2                16    // ต่อรับสัญญาณจาก micro:bit P8 (TX)
#define TXD2                17    // ต่อส่งสัญญาณไปยัง micro:bit P14 (RX)

// ================= ⏱️ ตัวแปรและเกณฑ์การตั้งเวลา (Scheduler & Cooldown) =================
const float TANK_EMPTY_DIST_CM = 30.0;     // ถ้าระยะผิวน้ำห่าง > 30 ซม. แปลว่าน้ำใกล้หมดถัง
const unsigned long SENSOR_INTERVAL = 2000; // รอบการอ่านเซนเซอร์ระดับน้ำ (2 วินาที)
const unsigned long CLOUD_INTERVAL  = 30000;// รอบการซิงค์ข้อมูลขึ้น Firebase (30 วินาที)
const unsigned long LINE_COOLDOWN_MS = 60000;// Cooldown LINE ป้องกัน Rate Limit (60 วินาที)

// ตัวแปรเวลา
unsigned long lastSensorCheck   = 0;
unsigned long lastFirebaseSync  = 0;

// ตัวแปรจับเวลาแยกสำหรับแต่ละสถานะของ LINE Alert
unsigned long lastLineStart     = 0;
unsigned long lastLineDone      = 0;
unsigned long lastLineLowWater  = 0;
unsigned long lastLineError     = 0;

// ตัวแปรสถานะระบบ
int currentSoilValue    = 0;
String currentPumpState = "IDLE";
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

  // วัดเวลาพัลส์สะท้อนกลับ (Timeout 30,000 ไมโครวินาที หรือประมาณ 5 เมตร)
  long duration = pulseIn(PIN_ECHO, HIGH, 30000);
  if (duration == 0) {
    return -1.0; // สัญญาณสะท้อนไม่กลับมา
  }
  return (duration * 0.0343) / 2.0;
}

// ================= 💬 ฟังก์ชันส่ง LINE Push Message (Messaging API แบบใหม่) =================
void sendLinePushMessage(String messageText) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[LINE]: Wi-Fi ยังไม่ได้เชื่อมต่อ ไม่สามารถส่งได้");
    return;
  }

  WiFiClientSecure client;
  client.setInsecure(); // ละเว้นการตรวจ CA certificate เพื่อความรวดเร็วและประหยัด RAM

  HTTPClient https;
  https.begin(client, "https://api.line.me/v2/bot/message/push");
  https.addHeader("Content-Type", "application/json");
  https.addHeader("Authorization", "Bearer " + String(LINE_CHANNEL_ACCESS_TOKEN));

  // สร้าง JSON Payload ตามมาตรฐาน LINE Messaging API v2
  String jsonBody = "{";
  jsonBody += "\"to\":\"" + String(LINE_USER_ID) + "\",";
  jsonBody += "\"messages\":[";
  jsonBody += "{\"type\":\"text\",\"text\":\"" + messageText + "\"}";
  jsonBody += "]";
  jsonBody += "}";

  int httpResponseCode = https.POST(jsonBody);
  if (httpResponseCode == 200) {
    Serial.println("[LINE Push]: ส่งข้อความสำเร็จ -> " + messageText);
  } else {
    Serial.print("[LINE Push]: เกิดข้อผิดพลาด HTTP Code: ");
    Serial.println(httpResponseCode);
    String response = https.getString();
    Serial.println("[LINE Response]: " + response);
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

  // สร้างเพย์โหลด JSON สอดคล้องกับ Security Rules
  String jsonPayload = "{";
  jsonPayload += "\"soil_moisture\":" + String(soil) + ",";
  jsonPayload += "\"water_distance_cm\":" + String(waterDist, 1) + ",";
  jsonPayload += "\"pump_state\":\"" + pumpState + "\",";
  jsonPayload += "\"timestamp\":{\".sv\":\"timestamp\"}";
  jsonPayload += "}";

  int httpCode = https.PUT(jsonPayload);
  if (httpCode == HTTP_CODE_OK) {
    Serial.println("[Firebase] ซิงค์ข้อมูลสำเร็จ");
  } else {
    Serial.println("[Firebase] ข้อผิดพลาด Code: " + String(httpCode));
  }
  https.end();
}

// ================= 🚀 Setup เริ่มต้นระบบ =================
void setup() {
  Serial.begin(115200);
  microbitSerial.begin(9600, SERIAL_8N1, RXD2, TXD2);

  pinMode(PIN_TRIG, OUTPUT);
  pinMode(PIN_ECHO, INPUT);

  Serial.println("\n--- เริ่มต้นระบบ Smart Auto-Watering LINE Bot Gateway ---");
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("กำลังเชื่อมต่อ Wi-Fi");

  int retries = 0;
  while (WiFi.status() != WL_CONNECTED && retries < 25) {
    delay(500);
    Serial.print(".");
    retries++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n[Wi-Fi] เชื่อมต่อสำเร็จ IP: " + WiFi.localIP().toString());
  } else {
    Serial.println("\n[Wi-Fi] เชื่อมต่อล้มเหลว กรุณาตรวจสอบ SSID/Password");
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
        currentPumpState = "RUNNING";
        if (now - lastLineStart >= LINE_COOLDOWN_MS) {
          sendLinePushMessage("🟢 ดินแห้ง เริ่มรดน้ำอัตโนมัติ (ความชื้นดิน: " + String(currentSoilValue) + ")");
          lastLineStart = now;
        }
      }
      else if (rawMsg.startsWith("ALERT:DONE")) {
        currentPumpState = "IDLE";
        if (now - lastLineDone >= LINE_COOLDOWN_MS) {
          sendLinePushMessage("🔵 รดน้ำเสร็จแล้ว (ความชื้นดิน: " + String(currentSoilValue) + ")");
          lastLineDone = now;
        }
      }
      else if (rawMsg.startsWith("ALERT:ERROR_TIMEOUT")) {
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
          currentSoilValue = rawMsg.substring(secondColon + 1).toInt();
        }
      }
    }
  }

  // 2. ตรวจสอบระดับน้ำในถังด้วยอัลตราโซนิกทุกๆ 2 วินาที (Non-blocking)
  if (now - lastSensorCheck >= SENSOR_INTERVAL) {
    lastSensorCheck = now;
    float dist = readUltrasonicDistance();

    if (dist > 0) {
      currentWaterDist = dist;

      // ตรวจสอบเงื่อนไขระดับน้ำต่ำ (Dry-run Protection)
      if (currentWaterDist >= TANK_EMPTY_DIST_CM) {
        if (!isWaterLow) {
          isWaterLow = true;
          // ส่งคำสั่ง Serial บังคับ micro:bit ดับปั๊มทันที
          microbitSerial.println("CMD:STOP");

          if (now - lastLineLowWater >= LINE_COOLDOWN_MS) {
            sendLinePushMessage("🔴 น้ำในถังใกล้หมด กรุณาเติมน้ำ (ระยะห่างผิวน้ำ: " + String(currentWaterDist, 1) + " ซม.)");
            lastLineLowWater = now;
          }
        }
      } else {
        if (isWaterLow) {
          isWaterLow = false;
          // ปลดล็อกระบบให้ micro:bit กลับมาทำงานได้ปกติ
          microbitSerial.println("CMD:RESUME");
        }
      }
    }
  }

  // 3. ส่งข้อมูลขึ้น Firebase Realtime Database ทุก 30 วินาที
  if (now - lastFirebaseSync >= CLOUD_INTERVAL) {
    lastFirebaseSync = now;
    syncToFirebase(currentSoilValue, currentWaterDist, currentPumpState);
  }
}
