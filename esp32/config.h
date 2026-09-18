#pragma once
#include <stdint.h>

// ================= ⚙️ Network & Vercel API Credentials =================
static const char WIFI_SSID[]       = "SinServer_2G";
static const char WIFI_PASSWORD[]   = "SinisterX";

// Web API endpoint สำหรับฟาร์มเกษตรบน Vercel
static const char API_URL[]         = "https://apiline.vercel.app/api/farm";
static const char API_KEY[]         = "dskruontop123";

// ================= 💬 LINE Messaging API =================
// Channel Access Token สำหรับส่ง Push Message
static const char LINE_TOKEN[]      = "wBhj9cj6teOS/gP9HrAzOk6no2xEC2rll7xKwu/ac8/b9pj9VOakW6Kh1DK+A/Yyl0XDBVpM+mSIA0c+/4DcDtliKvLWaucFrI4gSpR5HgH5GUJHr3WjW+DsdW4fdHfy1hyOXKeFVJTuQpHoEgnl2AdB04t89/1O/w1cDnyilFU=";

// Group ID หรือ User ID ปลายทาง
static const char LINE_GROUP_ID[]   = "Cec02d30682b66b1dfbca2f701bd77d52";
constexpr bool LINE_ENABLED         = true;

// ================= ☁️ Firebase Realtime Database (Production) =================
static const char FIREBASE_HOST[]   = "https://water-iot-prod-2026-default-rtdb.asia-southeast1.firebasedatabase.app";
static const char FIREBASE_AUTH[]   = "YOUR_DATABASE_SECRET_OR_WEB_API_KEY";

// ================= ⏱️ System & Protocol Parameters =================
// Soil sensor: micro:bit 10-bit ADC (0..1023)
constexpr bool SOIL_IS_RAW_ADC      = true;
// ส่งค่าสถานะปั๊มเป็นตัวเลข 0 หรือ 1 (ตาม Schema เซิร์ฟเวอร์)
constexpr bool API_PUMP_AS_BOOLEAN  = false;
// ปิด Extra Diagnostics ชั่วคราวตาม schema เซิร์ฟเวอร์
constexpr bool API_EXTRA_DIAGNOSTICS = false;

constexpr uint32_t DATA_STALE_MS    = 5000;
constexpr uint32_t API_INTERVAL_MS  = 5000;   // ส่ง API ขึ้น Vercel ทุกๆ 5 วินาที
constexpr uint32_t DIST_SEND_MS     = 500;    // อ่านระยะน้ำทุกๆ 500ms
constexpr int INVALID_DISTANCE      = 999;    // ค่าระยะผิดปกติ (micro:bit บังคับดับปั๊ม)
constexpr float TANK_EMPTY_DIST_CM  = 30.0;   // เกณฑ์ระยะน้ำแห้งก้นถัง (> 30 ซม.)
constexpr unsigned long LINE_COOLDOWN_MS = 60000; // Cooldown แจ้งเตือน LINE ป้องกันสแปม
