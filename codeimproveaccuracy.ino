/*
  ================================================================
   DO AN: HE THONG THEO DOI SUC KHOE IoT UNG DUNG EDGE COMPUTING
   PHIEN BAN TONG HOP: FREERTOS + PUBSUBCLIENT (QoS 0)
   (V4 - Đã fix lỗi Reset Watchdog & Lỗi Prototype)
  ================================================================
*/

#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SH110X.h>
#include "MAX30105.h"
#include "spo2_algorithm.h"
#include <WiFi.h>
#include <PubSubClient.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <math.h>

// ================================================================
// 1. ĐỊNH NGHĨA CÁC CẤU TRÚC DỮ LIỆU Ở TRÊN CÙNG
// ================================================================
enum UiType : uint8_t { UI_START, UI_MEASURING, UI_RESULT, UI_FAILED, UI_ERROR };

struct UiMsg {                    
  UiType  type;
  int16_t progress;               
  int16_t bpm;
  int16_t spo2;
  bool    abnormal;
};

struct HealthRecord {             
  int16_t  bpm;
  int16_t  spo2;
  bool     abnormal;
};

struct MeasurementRecord { 
  int bpm; 
  int spo2; 
  bool hasData; 
};

struct BiquadFilter { 
  float b0, b1, b2, a1, a2, x1, x2, y1, y2; 
};

// ================================================================
// 2. KHAI BÁO NGUYÊN MẪU HÀM (PROTOTYPES) 
// ================================================================
void setupButterworthLowpass(BiquadFilter &f, float fc, float fs);
void setupButterworthHighpass(BiquadFilter &f, float fc, float fs);
float processBiquad(BiquadFilter &f, float x0);
void drawResultScreen(const UiMsg &m);
void sendUi(UiType type, int progress = 0, int bpm = 0, int spo2 = 0, bool abnormal = false);


// ================================================================
// 3. CẤU HÌNH THÔNG SỐ
// ================================================================
const char* WIFI_SSID     = "TanPhuc";
const char* WIFI_PASSWORD = "09876543210";

#define MQTT_BROKER  "172.20.10.3"
#define MQTT_PORT    1883
#define MQTT_TOPIC   "tdtu/health/sensor"

#define BOT_TOKEN "8751361829:AAE4PW_cHmbOsds7KrJcLg0g80viKDfZQN0" 
#define CHAT_ID   "8926050085"

#define I2C_SDA_PIN        21
#define I2C_SCL_PIN        22
#define I2C_CLOCK_HZ       400000UL

#define SCREEN_WIDTH       128
#define SCREEN_HEIGHT      128
#define OLED_RESET         -1
#define OLED_I2C_ADDR      0x3C

#define BPM_LOW_LIMIT      60      
#define BPM_HIGH_LIMIT     100     
#define SPO2_LOW_LIMIT     95      
#define BPM_VALID_MIN      30
#define BPM_VALID_MAX      220
#define SPO2_VALID_MIN     70
#define SPO2_VALID_MAX     100
// ---- Bo loc gia toc tut nhip (BPM Delta) ----
#define BPM_DELTA_CHECK_BELOW    50      // chi kiem tra Delta khi nhip moi < 50
#define BPM_DELTA_MAX_DROP       25      // tut >= 25 nhip so voi lan do truoc => nghi sot dinh
#define DELTA_REJECT_AS_FAILED   0       // 0: giu lai nhip cu (dung yeu cau) | 1: bao FAILED de do lai


#define BUFFER_LENGTH      100     
#define FINGER_THRESHOLD   40000   
#define FINGER_LOST_TIMEOUT_MS   500UL
#define RESET_AFTER_ABSENT_MS    2000UL
#define DRAW_INTERVAL_MS         120UL
#define SENSOR_STALL_MS          3000UL   

#define HISTORY_SIZE 3
MeasurementRecord history[HISTORY_SIZE] = { {0, 0, false}, {0, 0, false}, {0, 0, false} };

// ------------------------- FREERTOS & HARDWARE OBJECTS -------------------------
Adafruit_SH1107 display = Adafruit_SH1107(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
MAX30105 particleSensor;

SemaphoreHandle_t i2cMutex    = nullptr;
QueueHandle_t     uiQueue     = nullptr;
QueueHandle_t     healthQueue = nullptr;

inline bool lockI2C(uint32_t timeoutMs = 100) {
  return xSemaphoreTake(i2cMutex, pdMS_TO_TICKS(timeoutMs)) == pdTRUE;
}
inline void unlockI2C() { xSemaphoreGive(i2cMutex); }

static bool isAbnormal(int bpm, int spo2) {
  return (bpm < BPM_LOW_LIMIT || bpm > BPM_HIGH_LIMIT || spo2 < SPO2_LOW_LIMIT);
}

static const char* statusLabel(int bpm, int spo2) {       
  if (bpm < BPM_LOW_LIMIT)  return "Nhip tim cham";
  if (bpm > BPM_HIGH_LIMIT) return "Nhip tim nhanh";
  if (spo2 < SPO2_LOW_LIMIT) return "SpO2 thap";
  return "Binh thuong";
}

// ================================================================
//        BỘ LỌC SỐ IIR BUTTERWORTH BẬC 2
// ================================================================
const float PPG_SAMPLE_RATE_HZ = 25.0f;
const float FILTER_LOWCUT_HZ   = 0.5f;
const float FILTER_HIGHCUT_HZ  = 4.0f;
const float SQRT2_CONST        = 1.41421356f;

BiquadFilter hpfIR, lpfIR, hpfRed, lpfRed;

void setupButterworthLowpass(BiquadFilter &f, float fc, float fs) {
  float k = tanf(PI * fc / fs);
  float norm = 1.0f / (1.0f + SQRT2_CONST * k + k * k);
  f.b0 = k * k * norm;  f.b1 = 2.0f * f.b0;  f.b2 = f.b0;
  f.a1 = 2.0f * (k * k - 1.0f) * norm;
  f.a2 = (1.0f - SQRT2_CONST * k + k * k) * norm;
  f.x1 = f.x2 = f.y1 = f.y2 = 0.0f;
}

void setupButterworthHighpass(BiquadFilter &f, float fc, float fs) {
  float k = tanf(PI * fc / fs);
  float norm = 1.0f / (1.0f + SQRT2_CONST * k + k * k);
  f.b0 = norm;  f.b1 = -2.0f * f.b0;  f.b2 = f.b0;
  f.a1 = 2.0f * (k * k - 1.0f) * norm;
  f.a2 = (1.0f - SQRT2_CONST * k + k * k) * norm;
  f.x1 = f.x2 = f.y1 = f.y2 = 0.0f;
}

float processBiquad(BiquadFilter &f, float x0) {
  float y0 = f.b0 * x0 + f.b1 * f.x1 + f.b2 * f.x2 - f.a1 * f.y1 - f.a2 * f.y2;
  f.x2 = f.x1;  f.x1 = x0;
  f.y2 = f.y1;  f.y1 = y0;
  return y0;
}

void resetFilterChain(long rawIR, long rawRed) {
  hpfIR.x1  = hpfIR.x2  = (float)rawIR;   hpfIR.y1  = hpfIR.y2  = 0.0f;
  hpfRed.x1 = hpfRed.x2 = (float)rawRed;  hpfRed.y1 = hpfRed.y2 = 0.0f;
  lpfIR.x1  = lpfIR.x2  = lpfIR.y1  = lpfIR.y2  = 0.0f;
  lpfRed.x1 = lpfRed.x2 = lpfRed.y1 = lpfRed.y2 = 0.0f;
}

// ================================================================
//                     CẤU HÌNH CẢM BIẾN MAX30102
// ================================================================
void configureSensor() {
  byte ledBrightness = 60;     
  byte sampleAverage = 4;      
  byte ledMode       = 2;      
  byte sampleRate    = 100;
  int  pulseWidth    = 411;    
  int  adcRange      = 4096;
  particleSensor.setup(ledBrightness, sampleAverage, ledMode, sampleRate, pulseWidth, adcRange);
}

bool recoverSensor() {
  if (!lockI2C(500)) return false;
  Serial.println(F("[SENSOR] Phat hien treo, dang khoi phuc I2C..."));
  Wire.end();
  delay(5);
  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN, I2C_CLOCK_HZ);
  bool ok = particleSensor.begin(Wire, I2C_SPEED_FAST);
  if (ok) configureSensor();
  unlockI2C();
  return ok;
}

void sendUi(UiType type, int progress, int bpm, int spo2, bool abnormal) {
  UiMsg m = { type, (int16_t)progress, (int16_t)bpm, (int16_t)spo2, abnormal };
  xQueueSend(uiQueue, &m, 0); 
}

// ================================================================
//        SENSOR TASK (CORE 1)
// ================================================================
enum SystemState { STATE_START, STATE_MEASURING, STATE_SUCCESS, STATE_FAILED };

uint32_t irBuffer[BUFFER_LENGTH];
uint32_t redBuffer[BUFFER_LENGTH];

void sensorTask(void *pv) {
  SystemState state = STATE_START;
  int      sampleIndex = 0;
  long     dcSnapshotIR = 0, dcSnapshotRed = 0;
  uint32_t lastFingerPresentMs = millis();
  uint32_t lastDrawMs = 0;
  uint32_t lastSampleMs = millis();

  // Khai báo thêm 2 biến cục bộ để triệt tiêu Race Condition và Timeout
  int           lastValidBpm  = 0;
  unsigned long lastValidTime = 0;

  sendUi(UI_START);

  for (;;) {
    if (lockI2C()) {
      particleSensor.check();
      unlockI2C();
    }

    bool gotSample = false;
    while (particleSensor.available()) {
      gotSample = true;
      lastSampleMs = millis();

      long irValue = particleSensor.getIR();       
      bool fingerNow = (irValue > FINGER_THRESHOLD);
      if (fingerNow) lastFingerPresentMs = millis();
      uint32_t absentMs = millis() - lastFingerPresentMs;

      switch (state) {
        case STATE_START:
          if (fingerNow) {
            sampleIndex = 0;
            state = STATE_MEASURING;
            sendUi(UI_MEASURING, 0);
          }
          break;

        case STATE_MEASURING:
          if (!fingerNow) {
            if (absentMs >= FINGER_LOST_TIMEOUT_MS) {
              state = STATE_FAILED;
              sendUi(UI_FAILED);
            }
          } else {
            long rawRed = particleSensor.getRed();
            if (sampleIndex == 0) {
              dcSnapshotIR = irValue;
              dcSnapshotRed = rawRed;
              resetFilterChain(irValue, rawRed);
            }
            float acIR  = processBiquad(lpfIR,  processBiquad(hpfIR,  (float)irValue));
            float acRed = processBiquad(lpfRed, processBiquad(hpfRed, (float)rawRed));
            long reconIR  = constrain((long)(acIR  + dcSnapshotIR),  0L, 262143L);
            long reconRed = constrain((long)(acRed + dcSnapshotRed), 0L, 262143L);
            irBuffer[sampleIndex]  = (uint32_t)reconIR;
            redBuffer[sampleIndex] = (uint32_t)reconRed;
            sampleIndex++;

            if (millis() - lastDrawMs > DRAW_INTERVAL_MS) {
              sendUi(UI_MEASURING, sampleIndex);   
              lastDrawMs = millis();
            }

            if (sampleIndex >= BUFFER_LENGTH) {
              int32_t spo2Value, heartRateValue;
              int8_t  spo2Valid, heartRateValid;
              maxim_heart_rate_and_oxygen_saturation(
                  irBuffer, BUFFER_LENGTH, redBuffer,
                  &spo2Value, &spo2Valid, &heartRateValue, &heartRateValid);

              bool hrHopLy      = (heartRateValue >= BPM_VALID_MIN && heartRateValue <= BPM_VALID_MAX);
              bool spo2HopLy    = (spo2Value >= SPO2_VALID_MIN && spo2Value <= SPO2_VALID_MAX);
              
              if ((heartRateValid || hrHopLy) && (spo2Valid || spo2HopLy)) {
                heartRateValue = constrain(heartRateValue, BPM_VALID_MIN, BPM_VALID_MAX);
                spo2Value      = constrain(spo2Value, SPO2_VALID_MIN, SPO2_VALID_MAX);

                // ---- Lọc Delta Nâng cấp (Chống Race Condition & Bắt lỗi chia đôi) ----
                bool rejectedByDelta = false;
                
                // Chỉ kiểm tra nếu nhịp < 50, có dữ liệu cũ, và khoảng cách giữa 2 lần đo dưới 60 giây
                if (heartRateValue < BPM_DELTA_CHECK_BELOW && lastValidBpm > 0 && (millis() - lastValidTime < 60000)) {
                  int delta = lastValidBpm - (int)heartRateValue;
                  
                  // Tính giá trị "bị chia đôi" lý thuyết
                  int expectedHalf = lastValidBpm / 2;
                  
                  // Điều kiện: Tụt mạnh (>= 25) VÀ giá trị mới loanh quanh mốc một nửa nhịp cũ (sai số +-5)
                  if (delta >= BPM_DELTA_MAX_DROP && abs((int)heartRateValue - expectedHalf) <= 5) {
                    Serial.printf("[DELTA] Bo %d BPM (tut %d so voi %d, dung dau hieu chia doi)\n",
                                  (int)heartRateValue, delta, lastValidBpm);
                    rejectedByDelta = true;
#if !DELTA_REJECT_AS_FAILED
                    heartRateValue = lastValidBpm; // Lấy nhịp cũ để lấp vào chỗ trống
#endif
                  }
                }

                if (rejectedByDelta && DELTA_REJECT_AS_FAILED) {
                  state = STATE_FAILED;
                  sendUi(UI_FAILED);
                } else {
                  // Cập nhật lại lịch sử cục bộ khi có một phép đo thành công
                  lastValidBpm = heartRateValue;
                  lastValidTime = millis();

                  bool abn = isAbnormal(heartRateValue, spo2Value);
                  state = STATE_SUCCESS;
                  sendUi(UI_RESULT, 0, heartRateValue, spo2Value, abn);

                  HealthRecord rec = {(int16_t)heartRateValue, (int16_t)spo2Value, abn};
                  
                  if (xQueueSend(healthQueue, &rec, 0) != pdTRUE) {   
                    HealthRecord dropped;
                    xQueueReceive(healthQueue, &dropped, 0); 
                    xQueueSend(healthQueue, &rec, 0);
                  }
                }
              } else {
                state = STATE_FAILED;
                sendUi(UI_FAILED);
              }
            }
          }
          break;

        case STATE_SUCCESS:
        case STATE_FAILED:
          if (absentMs >= RESET_AFTER_ABSENT_MS) {
            state = STATE_START;
            sendUi(UI_START);
          }
          break;
      }
      particleSensor.nextSample();
    }

    if (!gotSample && (millis() - lastSampleMs > SENSOR_STALL_MS)) {
      if (!recoverSensor()) sendUi(UI_ERROR);
      lastSampleMs = millis();
    }

    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

// ================================================================
//                CÁC HÀM VẼ OLED
// ================================================================
void pushHistory(int bpm, int spo2) {
  for (int i = HISTORY_SIZE - 1; i > 0; i--) history[i] = history[i - 1];
  history[0] = { bpm, spo2, true };
}

void flushDisplay() {
  if (lockI2C(200)) {
    display.display();            
    unlockI2C();
  }
}

void drawCenteredText(const String &text, int y, uint8_t textSize) {
  display.setTextSize(textSize);
  int16_t x1, y1; uint16_t w, h;
  display.getTextBounds(text, 0, y, &x1, &y1, &w, &h);
  display.setCursor((SCREEN_WIDTH - w) / 2, y);
  display.print(text);
}

void drawStartScreen() {
  display.clearDisplay();
  display.setTextColor(SH110X_WHITE);
  drawCenteredText("START", 2, 2);
  drawCenteredText("Dat ngon tay de do", 22, 1);
  display.drawFastHLine(4, 34, 120, SH110X_WHITE);
  drawCenteredText("LICH SU GAN NHAT", 40, 1);
  int rowY = 54;
  for (int i = 0; i < HISTORY_SIZE; i++) {
    String line = String(i + 1) + ": ";
    if (history[i].hasData) line += String(history[i].bpm) + "BPM - " + String(history[i].spo2) + "%";
    else                    line += "--BPM - --%";
    drawCenteredText(line, rowY, 1);
    rowY += 14;
  }
  flushDisplay();
}

void drawMeasuringScreen(int progress) {
  display.clearDisplay();
  display.setTextColor(SH110X_WHITE);
  drawCenteredText("Dang phan tich...", 20, 1);
  int barX = 10, barY = 55, barW = 108, barH = 16;
  display.drawRect(barX, barY, barW, barH, SH110X_WHITE);
  int fillW = constrain(map(progress, 0, BUFFER_LENGTH, 0, barW - 4), 0, barW - 4);
  display.fillRect(barX + 2, barY + 2, fillW, barH - 4, SH110X_WHITE);
  drawCenteredText(String(map(progress, 0, BUFFER_LENGTH, 0, 100)) + " %", 85, 1);
  flushDisplay();
}

void drawResultScreen(const UiMsg &m) {
  display.clearDisplay();
  display.setTextColor(SH110X_WHITE);
  drawCenteredText("KET QUA DO", 4, 1);
  display.drawFastHLine(6, 15, 116, SH110X_WHITE);
  drawCenteredText(String(m.bpm) + " BPM", 24, 2);
  drawCenteredText("SpO2: " + String(m.spo2) + " %", 50, 1);
  drawCenteredText(statusLabel(m.bpm, m.spo2), 66, 1);
  if (m.abnormal) {
    display.fillRect(4, 82, 120, 18, SH110X_WHITE);
    display.setTextColor(SH110X_BLACK);
    drawCenteredText("! CANH BAO !", 87, 1);
    display.setTextColor(SH110X_WHITE);
  }
  drawCenteredText("Rut tay de reset", 112, 1);
  flushDisplay();
}

void drawFailedScreen() {
  display.clearDisplay();
  display.setTextColor(SH110X_WHITE);
  drawCenteredText("Qua trinh", 40, 1);
  drawCenteredText("that bai", 55, 1);
  drawCenteredText("Rut tay de reset", 90, 1);
  flushDisplay();
}

void drawMessageScreen(const String &l1, const String &l2) {
  display.clearDisplay();
  display.setTextColor(SH110X_WHITE);
  drawCenteredText(l1, 50, 1);
  drawCenteredText(l2, 65, 1);
  flushDisplay();
}

// ================================================================
//                 DISPLAY TASK (CORE 1)
// ================================================================
void displayTask(void *pv) {
  UiMsg m;
  for (;;) {
    if (xQueueReceive(uiQueue, &m, pdMS_TO_TICKS(1000)) == pdTRUE) {
      switch (m.type) {
        case UI_START:     drawStartScreen(); break;
        case UI_MEASURING: drawMeasuringScreen(m.progress); break;
        case UI_RESULT:    pushHistory(m.bpm, m.spo2); drawResultScreen(m); break;
        case UI_FAILED:    drawFailedScreen(); break;
        case UI_ERROR:     drawMessageScreen("LOI CAM BIEN", "Kiem tra day I2C"); break;
      }
    }
  }
}

// ================================================================
//        NETWORK TASK (CORE 0): PUBSUBCLIENT & TELEGRAM POST
// ================================================================
WiFiClient   espClient;              
PubSubClient mqttClient(espClient);  

unsigned long lastWifiRetryMs = 0;
unsigned long lastMqttRetryMs = 0;

unsigned long lastTelegramSentMs = 0;
const unsigned long TELEGRAM_COOLDOWN_MS = 10000; 

void maintainConnections() {
  unsigned long now = millis();
  
  if (WiFi.status() != WL_CONNECTED) {
    if (now - lastWifiRetryMs >= 10000) {
      lastWifiRetryMs = now;
      Serial.println(F("[WiFi] Mat ket noi - thu lai"));
      WiFi.reconnect();
    }
    return;
  }

  if (!mqttClient.connected()) {
    if (now - lastMqttRetryMs >= 5000) {
      lastMqttRetryMs = now;
      Serial.print(F("[MQTT] Ket noi broker... "));
      String clientId = "ESP32-Health-" + String((uint32_t)ESP.getEfuseMac(), HEX);
      if (mqttClient.connect(clientId.c_str())) {
        Serial.println(F("OK"));
      } else {
        Serial.printf("that bai, rc=%d\n", mqttClient.state());
      }
    }
  } else {
    mqttClient.loop();
  }
}

void sendTelegramAlert(int bpm, int spo2) {
  unsigned long now = millis();
  if (lastTelegramSentMs != 0 && (now - lastTelegramSentMs < TELEGRAM_COOLDOWN_MS)) {
      Serial.println(F("[Telegram] Dang trong thoi gian cooldown 60s - Bo qua gui tin."));
      return;
  }

  Serial.println(F("[Telegram] Dang gui canh bao den Telegram..."));
  WiFiClientSecure clientSecure;
  clientSecure.setInsecure(); 
  
  HTTPClient http;
  String url = String("https://api.telegram.org/bot") + BOT_TOKEN + "/sendMessage";
  if (!http.begin(clientSecure, url)) {
      Serial.println(F("[Telegram] Loi khoi tao HTTP"));
      return;
  }
  
  http.setTimeout(8000); 
  http.addHeader("Content-Type", "application/json");

  char body[300];
  snprintf(body, sizeof(body),
           "{\"chat_id\":\"%s\",\"text\":\"CANH BAO Y TE KHAN CAP!\\nPhat hien chi so bat thuong:\\n- Nhip tim: %d BPM\\n- SpO2: %d %%\\nVui long kiem tra benh nhan ngay!\"}",
           CHAT_ID, bpm, spo2);
           
  int httpCode = http.POST((uint8_t *)body, strlen(body));
  http.end();
  
  if (httpCode == 200) {
      Serial.println(F("[Telegram] Gui tin nhan thanh cong!"));
      lastTelegramSentMs = millis(); 
  } else {
      Serial.printf("[Telegram] Gui that bai, ma loi: %d\n", httpCode);
  }
}

void networkTask(void *pv) {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  mqttClient.setServer(MQTT_BROKER, MQTT_PORT); 

  HealthRecord record;

  for (;;) {
    maintainConnections();

    if (xQueueReceive(healthQueue, &record, 0) == pdTRUE) {
        
        if (mqttClient.connected()) {
            String payload = "{\"bpm\": " + String(record.bpm) + ", \"spo2\": " + String(record.spo2) + "}";
            bool ok = mqttClient.publish(MQTT_TOPIC, payload.c_str(), false); 
            Serial.print(F("Publish MQTT ["));
            Serial.print(MQTT_TOPIC);
            Serial.print(F("]: "));
            Serial.println(ok ? F("(OK)") : F("(THAT BAI)"));
        }

        if (record.abnormal && WiFi.status() == WL_CONNECTED) {
            sendTelegramAlert(record.bpm, record.spo2);
        }
    }
    vTaskDelay(pdMS_TO_TICKS(50));
  }
}

// ================================================================
//                              SETUP & LOOP
// ================================================================

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println(F("\n=== HE THONG THEO DOI SUC KHOE IoT - V4 (FINAL) ==="));

  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN, I2C_CLOCK_HZ);

  i2cMutex    = xSemaphoreCreateMutex();
  uiQueue     = xQueueCreate(8, sizeof(UiMsg));
  healthQueue = xQueueCreate(10, sizeof(HealthRecord));

  if (!display.begin(OLED_I2C_ADDR, true)) {
    Serial.println(F("LOI: Khong tim thay man hinh OLED SH1107!"));
  }
  display.setRotation(0);
  display.clearDisplay();
  display.display();

  if (!particleSensor.begin(Wire, I2C_SPEED_FAST)) {
    Serial.println(F("LOI: Khong tim thay cam bien MAX30102!"));
    drawMessageScreen("LOI CAM BIEN", "Kiem tra day I2C");
    delay(5000);
    ESP.restart(); 
  }
  configureSensor();

  setupButterworthHighpass(hpfIR,  FILTER_LOWCUT_HZ,  PPG_SAMPLE_RATE_HZ);
  setupButterworthLowpass (lpfIR,  FILTER_HIGHCUT_HZ, PPG_SAMPLE_RATE_HZ);
  setupButterworthHighpass(hpfRed, FILTER_LOWCUT_HZ,  PPG_SAMPLE_RATE_HZ);
  setupButterworthLowpass (lpfRed, FILTER_HIGHCUT_HZ, PPG_SAMPLE_RATE_HZ);

  xTaskCreatePinnedToCore(sensorTask,  "sensor",  8192,  NULL, 3, NULL, 1);
  xTaskCreatePinnedToCore(displayTask, "display", 5120,  NULL, 2, NULL, 1);
  xTaskCreatePinnedToCore(networkTask, "network", 12288, NULL, 1, NULL, 0);
}

void loop() {
  // Thay vì xóa hàm loop() gây lỗi Watchdog, ta cho nó ngủ đông định kỳ
  vTaskDelay(pdMS_TO_TICKS(1000));
}