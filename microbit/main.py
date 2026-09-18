"""
Smart Auto-Watering System - micro:bit V2 Core Controller
Language: MicroPython
Author: Innovation Project Advisor & IoT Engineer
Description: ควบคุมการรดน้ำแปลงเกษตรอัตโนมัติ พร้อมระบบความปลอดภัย Timeout 60s และสื่อสาร UART กับ ESP32 แบบ Non-blocking
"""
from microbit import *
import utime

# ================= 📌 กำหนดขาเชื่อมต่อ (Pin Assignment) =================
PIN_SOIL = pin2       # เซนเซอร์วัดความชื้นในดิน (Analog IN: 0 - 1023)
PIN_RELAY = pin1      # ควบคุมโมดูลรีเลย์ (Digital OUT: 1 = ON, 0 = OFF)
PIN_DHT = pin0        # เซนเซอร์อุณหภูมิและความชื้น DHT11

# กำหนดพอร์ตสื่อสาร UART (P8 = TX ส่งข้อมูล, P14 = RX รับคำสั่ง)
uart.init(baudrate=9600, tx=pin8, rx=pin14)

# ================= ⚙️ ค่าคงที่และเกณฑ์การทำงาน (Thresholds) =================
SOIL_DRY_THRESHOLD = 850      # ค่าอ่านได้ > 850 แสดงว่าดินแห้ง ต้องเริ่มรดน้ำ
SOIL_WET_THRESHOLD = 700      # ค่าอ่านได้ < 700 แสดงว่าดินชุ่มชื้นพอแล้ว หยุดรดน้ำ
MAX_PUMP_TIME_MS = 60000      # Fail-safe: ตัดการทำงานหากปั๊มเปิดต่อเนื่องเกิน 60 วินาที
LOOP_INTERVAL_MS = 2000       # รอบการทำงานหลักทุกๆ 2 วินาที (Non-blocking)

# ================= 🚦 ตัวแปรสถานะระบบ (State Variables) =================
is_pumping = False
pump_start_time = 0
last_loop_time = 0
emergency_stop = False

# สั่งปิดรีเลย์เริ่มต้นเพื่อความปลอดภัย
PIN_RELAY.write_digital(0)
display.show(Image.ASLEEP)

def send_telemetry(header, soil_val):
    """ฟังก์ชันส่งข้อมูล telemetry ผ่าน UART ให้ ESP32"""
    msg = "{}:{}\n".format(header, soil_val)
    uart.write(msg)

# ================= 🔄 ลูปหลักของโปรแกรม (Main Loop) =================
while True:
    current_time = utime.ticks_ms()

    # 1. รับและประมวลผลคำสั่งฉุกเฉินจาก ESP32 ผ่าน UART
    if uart.any():
        raw_cmd = uart.readline()
        if raw_cmd:
            try:
                cmd_str = str(raw_cmd, 'UTF-8').strip()
                if "CMD:STOP" in cmd_str:
                    # กรณีน้ำในถังแห้ง บังคับปิดปั๊มทันที
                    PIN_RELAY.write_digital(0)
                    is_pumping = False
                    emergency_stop = True
                    display.show(Image.NO)
                    send_telemetry("STATUS:FORCED_STOP", PIN_SOIL.read_analog())
                elif "CMD:RESUME" in cmd_str:
                    # ปลดล็อกสถานะฉุกเฉินเมื่อเติมน้ำเรียบร้อย
                    emergency_stop = False
                    display.show(Image.YES)
            except Exception:
                pass

    # 2. ระบบป้องกันความปลอดภัย: ตรวจสอบเวลาทำงานต่อเนื่องของปั๊ม (Pump Timeout Protection)
    if is_pumping:
        elapsed_time = utime.ticks_diff(current_time, pump_start_time)
        if elapsed_time >= MAX_PUMP_TIME_MS:
            # ปิดปั๊มทันทีและเข้าสู่โหมดหยุดฉุกเฉิน
            PIN_RELAY.write_digital(0)
            is_pumping = False
            emergency_stop = True
            display.show(Image.SKULL)
            send_telemetry("ALERT:ERROR_TIMEOUT", PIN_SOIL.read_analog())

    # 3. ตรวจสอบเงื่อนไขการรดน้ำอัตโนมัติทุกๆ 2 วินาที (Non-blocking Scheduler)
    if utime.ticks_diff(current_time, last_loop_time) >= LOOP_INTERVAL_MS:
        last_loop_time = current_time
        soil_value = PIN_SOIL.read_analog()

        # ทำงานเฉพาะตอนที่ไม่ได้ติดสถานะหยุดฉุกเฉิน
        if not emergency_stop:
            # เงื่อนไขเริ่มรดน้ำ: ดินแห้ง (> 850) และปั๊มยังไม่ทำงาน
            if soil_value > SOIL_DRY_THRESHOLD and not is_pumping:
                PIN_RELAY.write_digital(1)
                is_pumping = True
                pump_start_time = utime.ticks_ms()
                display.show(Image.ARROW_N)
                send_telemetry("ALERT:START", soil_value)

            # เงื่อนไขหยุดรดน้ำ: ดินชื้นพอแล้ว (< 700) และปั๊มเปิดอยู่
            elif soil_value < SOIL_WET_THRESHOLD and is_pumping:
                PIN_RELAY.write_digital(0)
                is_pumping = False
                display.show(Image.HAPPY)
                send_telemetry("ALERT:DONE", soil_value)

            # ส่ง Heartbeat Telemetry ปกติ
            current_pump_label = "PUMP_ON" if is_pumping else "IDLE"
            send_telemetry("DATA:" + current_pump_label, soil_value)
