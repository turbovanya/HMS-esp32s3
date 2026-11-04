#include <Wire.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <Adafruit_SHT4x.h>
#include <Adafruit_BMP280.h>
#include <SensirionI2CSgp40.h>
#include <SensirionGasIndexAlgorithm.h>
#include <esp_task_wdt.h>
#include <esp_system.h>

#include "Gfx.h"
#include "Ui.h"
#include "History.h"

// =====================================================
// ПИНЫ
// =====================================================
#define TFT_SCLK 36
#define TFT_MOSI 35
#define TFT_CS   37
#define TFT_DC   38
#define TFT_RST  39
#define I2C_SDA  41
#define I2C_SCL  42
#define PMS_RX   2
#define MHZ19_RX 16
#define MHZ19_TX 15

#define SCREEN_W 320
#define SCREEN_H 240

// =====================================================
// СТРУКТУРЫ
// =====================================================
struct SensorValue {
  float value = NAN;
  bool ok = false;
  unsigned long lastGood = 0;
};

// =====================================================
// ОБЪЕКТЫ
// ВАЖНО: конструктор БЕЗ MOSI/SCLK = аппаратный SPI
// =====================================================
Adafruit_ST7789 tft = Adafruit_ST7789(TFT_CS, TFT_DC, TFT_RST);
Adafruit_SHT4x sht40 = Adafruit_SHT4x();
Adafruit_BMP280 bmp;
SensirionI2CSgp40 sgp40;
GasIndexAlgorithmParams voc_algorithm_params;
HardwareSerial SerialPMS(1);
HardwareSerial SerialCO2(2);

gfx::Canvas canvas;
UiData ui;

// =====================================================
// ДАННЫЕ
// =====================================================
SensorValue vTemp, vHum, vPres, vVoc, vCo2, vPm1, vPm25, vPm10;
bool sht40_ok = false, bmp_ok = false, sgp40_ok = false;
const unsigned long STALE_MS = 8000;

uint8_t pmsBuf[32];
uint8_t pmsIdx = 0;
bool pmsHeader = false;

enum Co2State { CO2_IDLE, CO2_WAIT_RESP };
Co2State co2State = CO2_IDLE;
uint8_t co2Buf[9];
uint8_t co2Idx = 0;
unsigned long co2RequestSent = 0;
const unsigned long CO2_TIMEOUT_MS = 250;

// =====================================================
// ИСТОРИЯ ДЛЯ ГРАФИКОВ И ТРЕНДОВ
// Отсчёт каждые 10 с, 360 отсчётов = 1 час.
// Давление: отсчёт раз в 5 минут, 37 отсчётов = 3 часа.
// =====================================================
const unsigned long SAMPLE_MS = 10000;
const unsigned long PRES_SAMPLE_MS = 300000;
History<360> hCo2, hVoc;
History<37> hPres;

// =====================================================
// ЭКРАН
// =====================================================
const unsigned long FRAME_MS = 80;
bool shotRequested = false;
int demoLevel = -2;  // -2 реальные данные, -1 демо по кругу, 0..4 фиксированный уровень
unsigned long lastFrameUs = 0, lastDrawUs = 0;

// =====================================================
// СГЛАЖИВАНИЕ
// =====================================================
float ema(float prevVal, float newVal, float alpha) {
  if (isnan(prevVal)) return newVal;
  return prevVal + alpha * (newVal - prevVal);
}
const float ALPHA_CLIMATE = 0.25f;
const float ALPHA_PM      = 0.35f;

// =====================================================
// ВСПОМОГАТЕЛЬНОЕ
// =====================================================
void setStale(SensorValue &v) {
  if (v.ok && millis() - v.lastGood > STALE_MS) v.ok = false;
}

// =====================================================
// SETUP
// =====================================================
void setup() {
  Serial.begin(115200);
  Serial.setTxTimeoutMs(0);  // без компьютера вывод не должен тормозить цикл
  delay(300);

  esp_reset_reason_t reason = esp_reset_reason();
  Serial.print(F("Reset reason code: "));
  Serial.println((int)reason);

#if defined(ESP_ARDUINO_VERSION) && defined(ESP_ARDUINO_VERSION_VAL)
#if ESP_ARDUINO_VERSION >= ESP_ARDUINO_VERSION_VAL(3, 0, 0)
  esp_task_wdt_config_t wdt_config = {
    .timeout_ms = 8000,
    .idle_core_mask = 0,
    .trigger_panic = true
  };
  esp_task_wdt_init(&wdt_config);
#else
  esp_task_wdt_init(8, true);
#endif
#else
  esp_task_wdt_init(8, true);
#endif
  esp_task_wdt_add(NULL);

  // Буфер кадра выделяем до всего остального, пока память не раздроблена
  bool canvasOk = canvas.begin(SCREEN_W, SCREEN_H);

  // Аппаратный сброс TFT (убирает синий экран при старте)
  pinMode(TFT_RST, OUTPUT);
  digitalWrite(TFT_RST, LOW);
  delay(50);
  digitalWrite(TFT_RST, HIGH);
  delay(150);

  SPI.begin(TFT_SCLK, -1, TFT_MOSI, TFT_CS);
  tft.init(240, 320);
  tft.setSPISpeed(40000000);
  tft.setRotation(1);
  tft.invertDisplay(false);
  tft.fillScreen(0x0000);
  tft.setTextWrap(false);

  if (!canvasOk) {
    tft.setTextColor(0xF800);
    tft.setCursor(10, 10);
    tft.print(F("No memory for frame buffer"));
  }
  Serial.printf("Frame band: %d rows, free heap: %u\n", canvas.bandHeight(), (unsigned)ESP.getFreeHeap());

  Wire.begin(I2C_SDA, I2C_SCL);
  Wire.setTimeOut(50);

  SerialPMS.begin(9600, SERIAL_8N1, PMS_RX, -1);
  SerialCO2.begin(9600, SERIAL_8N1, MHZ19_RX, MHZ19_TX);

  sht40_ok = sht40.begin();
  bmp_ok   = bmp.begin(0x76);
  sgp40.begin(Wire);
  sgp40_ok = true;

  GasIndexAlgorithm_init(&voc_algorithm_params, GasIndexAlgorithm_ALGORITHM_TYPE_VOC);

  if (!sht40_ok) Serial.println(F("SHT40 ERR"));
  if (!bmp_ok)   Serial.println(F("BMP280 ERR"));

  Serial.println(F("System Ready"));
}

// =====================================================
// LOOP
// =====================================================
void loop() {
  esp_task_wdt_reset();
  readClimate();
  readSGP40();
  pollCO2();
  readPMS();
  retrySensorInit();
  sampleHistory();
  handleSerial();

  static unsigned long lastFrame = 0;
  if (millis() - lastFrame >= FRAME_MS) {
    lastFrame = millis();
    setStale(vTemp); setStale(vHum); setStale(vPres);
    setStale(vVoc);  setStale(vCo2);
    setStale(vPm1);  setStale(vPm25); setStale(vPm10);
    buildUiData();
    if (demoLevel > -2) applyDemo();
    // Кадр рисуем только когда картинка изменилась: меньше нагрева рядом с датчиками
    static unsigned long lastRender = 0;
    bool dirty = uiUpdate(ui, millis());
    if (canvas.buffer() && (dirty || shotRequested || millis() - lastRender > 5000)) {
      renderFrame();
      lastRender = millis();
    }
  }
}

// =====================================================
// ОТРИСОВКА КАДРА
// =====================================================
void renderFrame() {
  unsigned long t0 = micros();
  bool shot = shotRequested;
  shotRequested = false;
  if (shot) {
    Serial.setTxTimeoutMs(2000);
    Serial.printf("SHOT %d %d\n", SCREEN_W, SCREEN_H);
  }
  unsigned long drawUs = 0;
  for (int b = 0; b < canvas.bandCount(); b++) {
    canvas.setBand(b);
    unsigned long d0 = micros();
    uiDraw(canvas, ui, millis());
    drawUs += micros() - d0;
    tft.startWrite();
    tft.setAddrWindow(0, canvas.bandY(), canvas.width(), canvas.bandRows());
    tft.writePixels(canvas.buffer(), (uint32_t)canvas.width() * canvas.bandRows(), true, false);
    tft.endWrite();
    if (shot) Serial.write((const uint8_t*)canvas.buffer(), (size_t)canvas.width() * canvas.bandRows() * 2);
    esp_task_wdt_reset();
  }
  if (shot) {
    Serial.flush();
    Serial.print(F("\nEND\n"));
    Serial.setTxTimeoutMs(0);
  }
  lastFrameUs = micros() - t0;
  lastDrawUs = drawUs;
}

// =====================================================
// КОМАНДЫ ПО USB
// s - снимок экрана, d - демо по кругу, 0..4 - демо уровня, r - реальные данные, i - инфо
// =====================================================
void handleSerial() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == 's') shotRequested = true;
    else if (c == 'd') demoLevel = -1;
    else if (c >= '0' && c <= '4') demoLevel = c - '0';
    else if (c == 'r') demoLevel = -2;
    else if (c == 'i') {
      Serial.setTxTimeoutMs(200);
      Serial.printf("band=%d draw=%lu us frame=%lu us heap=%u minheap=%u uptime=%lu s\n",
                    canvas.bandHeight(), lastDrawUs, lastFrameUs, (unsigned)ESP.getFreeHeap(),
                    (unsigned)ESP.getMinFreeHeap(), millis() / 1000);
      Serial.flush();
      Serial.setTxTimeoutMs(0);
    }
  }
}

// =====================================================
// ДАННЫЕ ДЛЯ ЭКРАНА
// =====================================================
void toMetric(Metric& m, const SensorValue& v, unsigned long warmupMs) {
  m.value = v.value;
  if (v.ok) m.state = S_OK;
  else if (v.lastGood == 0 && millis() < warmupMs) m.state = S_WARMUP;
  else m.state = S_LOST;
}

void buildUiData() {
  toMetric(ui.temp, vTemp, 15000);
  toMetric(ui.hum,  vHum,  15000);
  toMetric(ui.pres, vPres, 15000);
  toMetric(ui.co2,  vCo2,  180000);
  toMetric(ui.voc,  vVoc,  120000);
  toMetric(ui.pm1,  vPm1,  45000);
  toMetric(ui.pm25, vPm25, 45000);
  toMetric(ui.pm10, vPm10, 45000);

  // CO2 и VOC: тренд за 15 минут
  fillTrend(ui.co2Trend, hCo2, 1.0f, 90, 12);
  fillTrend(ui.vocTrend, hVoc, 1.0f, 90, 12);
  // Давление: изменение за 3 часа, оценка появляется через 30 минут
  fillTrend(ui.presTrend, hPres, 0.1f, 36, 7, true);
}

void sampleHistory() {
  static unsigned long last = 0, lastPres = 0;
  unsigned long now = millis();
  if (now - last >= SAMPLE_MS) {
    last = now;
    if (vCo2.ok)  hCo2.push((int16_t)lroundf(vCo2.value));
    if (vVoc.ok)  hVoc.push((int16_t)lroundf(vVoc.value));
  }
  if (vPres.ok && (hPres.count == 0 || now - lastPres >= PRES_SAMPLE_MS)) {
    lastPres = now;
    hPres.push((int16_t)lroundf(vPres.value * 10));
  }
}

// =====================================================
// ДЕМО (для проверки цветов и вёрстки без реального воздуха)
// =====================================================
void demoTrend(Trend& t, float end, float amp, float slope) {
  t.count = SPARK_POINTS;
  for (int j = 0; j < SPARK_POINTS; j++) {
    float k = (float)j / (SPARK_POINTS - 1);
    t.points[j] = end * (1.0f - slope * (1.0f - k)) + amp * sinf(j * 0.45f) * (1.0f - k * 0.6f);
  }
  t.points[SPARK_POINTS - 1] = end;
  t.delta = end - t.points[SPARK_POINTS - 1 - 18];
  t.valid = true;
}

void applyDemo() {
  struct Sc { float co2, voc, pm1, pm25, pm10; };
  static const Sc SC[5] = {
    {520, 85, 3, 5, 7}, {900, 130, 9, 18, 24}, {1250, 210, 22, 30, 45},
    {1700, 330, 35, 48, 80}, {2600, 450, 60, 90, 160}
  };
  Sc s;
  if (demoLevel >= 0) {
    s = SC[demoLevel];
  } else {
    float p = fmodf(millis() / 6000.0f, 8.0f);
    float k = p < 4 ? p : 8 - p;
    int i = min(3, (int)k);
    float f = k - i;
    s.co2  = SC[i].co2  + (SC[i + 1].co2  - SC[i].co2)  * f;
    s.voc  = SC[i].voc  + (SC[i + 1].voc  - SC[i].voc)  * f;
    s.pm1  = SC[i].pm1  + (SC[i + 1].pm1  - SC[i].pm1)  * f;
    s.pm25 = SC[i].pm25 + (SC[i + 1].pm25 - SC[i].pm25) * f;
    s.pm10 = SC[i].pm10 + (SC[i + 1].pm10 - SC[i].pm10) * f;
  }
  Metric* all[] = {&ui.temp, &ui.hum, &ui.pres, &ui.co2, &ui.voc, &ui.pm1, &ui.pm25, &ui.pm10};
  for (Metric* m : all) m->state = S_OK;
  ui.co2.value = s.co2;  ui.voc.value = s.voc;
  ui.pm1.value = s.pm1;  ui.pm25.value = s.pm25; ui.pm10.value = s.pm10;
  ui.temp.value = 23.4f; ui.hum.value = 46; ui.pres.value = 748;
  demoTrend(ui.co2Trend, s.co2, s.co2 * 0.06f, 0.35f);
  demoTrend(ui.vocTrend, s.voc, s.voc * 0.08f, -0.2f);
  ui.presTrend.valid = true; ui.presTrend.delta = -2.1f;
}

// =====================================================
// РЕТРАЙ ИНИЦИАЛИЗАЦИИ
// =====================================================
void retrySensorInit() {
  static unsigned long lastRetry = 0;
  if (millis() - lastRetry < 10000) return;
  lastRetry = millis();
  if (!sht40_ok) sht40_ok = sht40.begin();
  if (!bmp_ok)   bmp_ok   = bmp.begin(0x76);
}

// =====================================================
// КЛИМАТ
// =====================================================
void readClimate() {
  static unsigned long last = 0;
  if (millis() - last < 2000) return;
  last = millis();

  if (sht40_ok) {
    sensors_event_t h, t;
    sht40.getEvent(&h, &t);
    if (!isnan(t.temperature)) {
      vTemp.value = ema(vTemp.value, t.temperature, ALPHA_CLIMATE);
      vTemp.ok = true; vTemp.lastGood = millis();
      vHum.value = ema(vHum.value, h.relative_humidity, ALPHA_CLIMATE);
      vHum.ok = true; vHum.lastGood = millis();
    }
  }
  if (bmp_ok) {
    float p = bmp.readPressure();
    if (!isnan(p) && p > 30000) {
      vPres.value = ema(vPres.value, p * 0.00750062f, ALPHA_CLIMATE);
      vPres.ok = true; vPres.lastGood = millis();
    }
  }
}

// =====================================================
// SGP40 VOC
// =====================================================
void readSGP40() {
  static unsigned long last = 0;
  if (millis() - last < 1000) return;
  last = millis();

  if (!sgp40_ok || !vTemp.ok || !vHum.ok) return;
  uint16_t rawSignal;
  if (sgp40.measureRawSignal(vTemp.value, vHum.value, rawSignal) == 0) {
    int32_t voc_index;
    GasIndexAlgorithm_process(&voc_algorithm_params, rawSignal, &voc_index);
    // Первые ~45 с алгоритм отдаёт 0, это ещё прогрев
    if (voc_index > 0) {
      vVoc.value = voc_index; vVoc.ok = true; vVoc.lastGood = millis();
    }
  }
}

// =====================================================
// MH-Z19B
// =====================================================
void pollCO2() {
  static unsigned long lastRequest = 0;
  if (co2State == CO2_IDLE) {
    if (millis() - lastRequest < 5000) return;
    while (SerialCO2.available()) SerialCO2.read();
    byte cmd[9] = {0xFF, 0x01, 0x86, 0x00, 0x00, 0x00, 0x00, 0x00, 0x79};
    SerialCO2.write(cmd, 9);
    co2Idx = 0;
    co2RequestSent = millis();
    co2State = CO2_WAIT_RESP;
    lastRequest = millis();
    return;
  }

  while (SerialCO2.available() && co2Idx < 9) {
    co2Buf[co2Idx++] = SerialCO2.read();
  }

  if (co2Idx >= 9) {
    byte checksum = 0;
    for (int i = 1; i < 8; i++) checksum += co2Buf[i];
    checksum = 0xff - checksum + 1;
    if (co2Buf[8] == checksum && co2Buf[0] == 0xFF && co2Buf[1] == 0x86) {
      vCo2.value = 256 * (int)co2Buf[2] + co2Buf[3];
      vCo2.ok = true; vCo2.lastGood = millis();
    }
    co2State = CO2_IDLE;
  } else if (millis() - co2RequestSent > CO2_TIMEOUT_MS) {
    co2State = CO2_IDLE;
  }
}

// =====================================================
// PMS7003
// =====================================================
void readPMS() {
  while (SerialPMS.available()) {
    uint8_t b = SerialPMS.read();
    if (!pmsHeader) {
      if (pmsIdx == 0 && b == 0x42) pmsBuf[pmsIdx++] = b;
      else if (pmsIdx == 1 && b == 0x4D) { pmsBuf[pmsIdx++] = b; pmsHeader = true; }
      else pmsIdx = 0;
    } else {
      pmsBuf[pmsIdx++] = b;
      if (pmsIdx == 32) {
        uint16_t sum = 0;
        for (int i = 0; i < 30; i++) sum += pmsBuf[i];
        if (sum == ((pmsBuf[30] << 8) | pmsBuf[31])) {
          float raw1  = (pmsBuf[10] << 8) | pmsBuf[11];
          float raw25 = (pmsBuf[12] << 8) | pmsBuf[13];
          float raw10 = (pmsBuf[14] << 8) | pmsBuf[15];
          vPm1.value  = ema(vPm1.value, raw1, ALPHA_PM);  vPm1.ok  = true; vPm1.lastGood  = millis();
          vPm25.value = ema(vPm25.value, raw25, ALPHA_PM); vPm25.ok = true; vPm25.lastGood = millis();
          vPm10.value = ema(vPm10.value, raw10, ALPHA_PM); vPm10.ok = true; vPm10.lastGood = millis();
        }
        pmsIdx = 0;
        pmsHeader = false;
      }
    }
  }
}
