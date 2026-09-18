/**
 * Project: Smart Auto-Watering & LINE Alert System
 * Device: ESP32 IoT Gateway (NodeMCU-32S)
 * Features:
 *   - Non-blocking millis() multitasking loop
 *   - LINE Notify rate-limiting cooldown (60s / state)
 *   - Firebase Realtime Database cloud sync (30s interval)
 *   - Ultrasonic HC-SR04 dry-run protection (< 30cm safety)
 *   - Full UART bidirectional communication with micro:bit V2
 */

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HardwareSerial.h>
#include <HTTPClient.h>

// ================= ⚙️ การตั้งค่าระบบเครือข่ายและ Cloud =================
const char* WIFI_SSID       = "YOUR_WIFI_SSID";
const char* WIFI_PASSWORD   = "YOUR_WIFI_PASSWORD";

// รับ Token จาก https://notify-bot.line.me/
const char* LINE_TOKEN      = "YOUR_LINE_NOTIFY_TOKEN";

// Firebase Realtime Database Configuration
const char* FIREBASE_HOST   = "https://your-project-default-rtdb.asia-southeast1.firebasedatabase.app";
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

// ตัวแปรจับเวลาแยกสำหรับแต่ละสถานะของ LINE Notify
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
  // คำนวณเป็นหน่วยเซนติเมตร: ระยะทาง = (เวลา * ความเร็วเสียง 0.0343 ซม./us) / 2
  return (duration * 0.0343) / 2.0;
}

// ================= 💬 ฟังก์ชันส่งข้อความแจ้งเตือนผ่าน LINE Notify =================
void sendLineNotify(String message) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[LINE]: Wi-Fi ยังไม่ได้เชื่อมต่อ ไม่สามารถส่งได้");
    return;
  }

  WiFiClientSecure client;
  client.setInsecure(); // ละเว้นการตรวจสอบ SSL cert เพื่อประสิทธิภาพการทำงานบน MCU

  HTTPClient https;
  https.begin(client, "https://notify-api.line.me/api/notify");
  https.addHeader("Content-Type", "application/x-www-form-urlencoded");
  https.addHeader("Authorization", "Bearer " + String(LINE_TOKEN));

  String payload = "message=" + message;
  int httpCode = https.POST(payload);
  if (httpCode > 0) {
    Serial.println("[LINE] ส่งสำเร็จ: " + message);
  } else {
    Serial.println("[LINE] ส่งล้มเหลว Code: " + String(httpCode));
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

  // สร้างเพย์โหลด JSON
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
    Serial.println("[Firebase] เกิดข้อผิดพลาด Code: " + String(httpCode));
  }
  https.end();
}

// ================= 🚀 Setup เริ่มต้นระบบ =================
void setup() {
  Serial.begin(115200);
  microbitSerial.begin(9600, SERIAL_8N1, RXD2, TXD2);

  pinMode(PIN_TRIG, OUTPUT);
  pinMode(PIN_ECHO, INPUT);

  Serial.println("\n--- เริ่มต้นระบบ Smart Auto-Watering Gateway ---");
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
          sendLineNotify("🟢 ดินแห้ง เริ่มรดน้ำอัตโนมัติ (ความชื้นดิน: " + String(currentSoilValue) + ")");
          lastLineStart = now;
        }
      }
      else if (rawMsg.startsWith("ALERT:DONE")) {
        currentPumpState = "IDLE";
        if (now - lastLineDone >= LINE_COOLDOWN_MS) {
          sendLineNotify("🔵 รดน้ำเสร็จแล้ว (ความชื้นดิน: " + String(currentSoilValue) + ")");
          lastLineDone = now;
        }
      }
      else if (rawMsg.startsWith("ALERT:ERROR_TIMEOUT")) {
        currentPumpState = "ERROR";
        if (now - lastLineError >= LINE_COOLDOWN_MS) {
          sendLineNotify("⚠️ ระบบผิดปกติ: ปั๊มน้ำทำงานเกิน 60 วินาที ตัดระบบฉุกเฉิน โปรดตรวจสอบแปลงเกษตร");
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
            sendLineNotify("🔴 น้ำในถังใกล้หมด กรุณาเติมน้ำ (ระยะห่างผิวน้ำ: " + String(currentWaterDist, 1) + " ซม.)");
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
