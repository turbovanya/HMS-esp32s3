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

// =====================================================
// СТРУКТУРЫ
// =====================================================
struct SensorValue {
  float value = NAN;
  bool ok = false;
  unsigned long lastGood = 0;
};

struct Card {
  int x, y, w, h;
  const char* label;
  const char* unit;
};

// =====================================================
// ПАЛИТРА (контраст по WCAG AAA ≥ 7:1)
// =====================================================
uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
  return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}

const uint16_t COL_BG        = rgb565(8, 9, 12);
const uint16_t COL_CARD_BG   = rgb565(22, 25, 32);
const uint16_t COL_BORDER    = rgb565(48, 52, 62);
const uint16_t COL_TEXT      = rgb565(255, 255, 255);
const uint16_t COL_MUTED     = rgb565(140, 148, 162);
const uint16_t COL_GOOD      = rgb565(48, 209, 88);
const uint16_t COL_WARN      = rgb565(255, 214, 10);
const uint16_t COL_BAD       = rgb565(255, 69, 58);
const uint16_t COL_STRIPE_BG = rgb565(38, 42, 52);

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
// СГЛАЖИВАНИЕ
// =====================================================
float ema(float prevVal, float newVal, float alpha) {
  if (isnan(prevVal)) return newVal;
  return prevVal + alpha * (newVal - prevVal);
}
const float ALPHA_CLIMATE = 0.25f;
const float ALPHA_PM      = 0.35f;

// =====================================================
// СЕТКА (без gauge - место отдано цифрам)
// =====================================================
Card cTemp = {4,   4,   100, 50, "TEMP",  "C"};
Card cHum  = {108, 4,   100, 50, "HUM",   "%"};
Card cPres = {212, 4,   104, 50, "BARO",  "mmHg"};

Card cCo2  = {4,   58,  154, 80, "CO2",       "ppm"};
Card cVoc  = {162, 58,  154, 80, "VOC INDEX", "idx"};

Card cPm1  = {4,   142, 100, 94, "PM1.0", "ug/m3"};
Card cPm25 = {108, 142, 100, 94, "PM2.5", "ug/m3"};
Card cPm10 = {212, 142, 104, 94, "PM10",  "ug/m3"};

// =====================================================
// КЭШ ПЕРЕРИСОВКИ (цифра + цвет левой полосы отдельно)
// =====================================================
enum CardId { ID_TEMP, ID_HUM, ID_PRES, ID_CO2, ID_VOC, ID_PM1, ID_PM25, ID_PM10, CARD_COUNT };
char     cardLastText[CARD_COUNT][10];
uint16_t cardLastColor[CARD_COUNT];
uint16_t cardLastStripe[CARD_COUNT];
bool     cardInited[CARD_COUNT];

bool needsRedrawValue(int id, const char* text, uint16_t color) {
  if (!cardInited[id] || color != cardLastColor[id] ||
      strncmp(cardLastText[id], text, sizeof(cardLastText[id])) != 0) {
    strncpy(cardLastText[id], text, sizeof(cardLastText[id]) - 1);
    cardLastText[id][sizeof(cardLastText[id]) - 1] = 0;
    cardLastColor[id] = color;
    cardInited[id] = true;
    return true;
  }
  return false;
}

bool needsRedrawStripe(int id, uint16_t color) {
  if (!cardInited[id] || color != cardLastStripe[id]) {
    cardLastStripe[id] = color;
    cardInited[id] = true;
    return true;
  }
  return false;
}

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

  // Аппаратный сброс TFT (убирает синий экран при старте)
  pinMode(TFT_RST, OUTPUT);
  digitalWrite(TFT_RST, LOW);
  delay(50);
  digitalWrite(TFT_RST, HIGH);
  delay(150); // Увеличенная задержка - даём контроллеру TFT стабилизироваться

  // АППАРАТНЫЙ SPI - ключ к отсутствию мерцания
  SPI.begin(TFT_SCLK, -1, TFT_MOSI, TFT_CS);
  tft.init(240, 320);
  tft.setSPISpeed(40000000); // 40 МГц = ~1 мс на кадр вместо ~40 мс

  tft.setRotation(1);
  tft.fillScreen(COL_BG);
  tft.invertDisplay(false);   // Явно выключаем инверсию (лечит синий оттенок)
  tft.setTextWrap(false);

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

  drawStaticUI();
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

  static unsigned long lastDisplay = 0;
  if (millis() - lastDisplay > 400) {
    setStale(vTemp); setStale(vHum); setStale(vPres);
    setStale(vVoc);  setStale(vCo2);
    setStale(vPm1);  setStale(vPm25); setStale(vPm10);
    updateDisplay();
    lastDisplay = millis();
  }
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
    vVoc.value = voc_index; vVoc.ok = true; vVoc.lastGood = millis();
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

// =====================================================
// ЦВЕТ ПО ПОРОГУ
// =====================================================
uint16_t thresholdColor(float v, float warnT, float badT) {
  if (v > badT) return COL_BAD;
  if (v > warnT) return COL_WARN;
  return COL_GOOD;
}

// =====================================================
// СТАТИЧНАЯ ЧАСТЬ UI
// =====================================================
void drawCardFrame(Card &c) {
  // Левая цветная полоса (статус) - 4px шириной
  tft.fillRect(c.x, c.y, 4, c.h, COL_STRIPE_BG);
  // Основная часть карточки
  tft.drawRoundRect(c.x + 4, c.y, c.w - 4, c.h, 6, COL_BORDER);
  tft.fillRoundRect(c.x + 5, c.y + 1, c.w - 6, c.h - 2, 5, COL_CARD_BG);
  // Заголовок слева
  tft.setTextSize(1);
  tft.setTextColor(COL_MUTED, COL_CARD_BG);
  tft.setCursor(c.x + 12, c.y + 6);
  tft.print(c.label);
  // Единицы справа
  int unitLen = strlen(c.unit);
  tft.setCursor(c.x + c.w - 7 - unitLen * 6, c.y + 6);
  tft.print(c.unit);
}

void drawStaticUI() {
  tft.fillScreen(COL_BG);
  drawCardFrame(cTemp);
  drawCardFrame(cHum);
  drawCardFrame(cPres);
  drawCardFrame(cCo2);
  drawCardFrame(cVoc);
  drawCardFrame(cPm1);
  drawCardFrame(cPm25);
  drawCardFrame(cPm10);
}

// =====================================================
// ОТРИСОВКА ЗНАЧЕНИЯ (перезаливка только области цифры)
// =====================================================
void drawBigValue(Card &c, int yOff, const char* text, uint16_t color, int textSize) {
  int textH = textSize * 8;
  // Область цифры: от левой полосы до правого края
  tft.fillRect(c.x + 5, c.y + yOff, c.w - 6, textH + 2, COL_CARD_BG);
  tft.setTextSize(textSize);
  tft.setTextColor(color, COL_CARD_BG);
  tft.setCursor(c.x + 12, c.y + yOff);
  tft.print(text);
}

// =====================================================
// ЦВЕТНАЯ ЛЕВАЯ ПОЛОСА (светофор для периферийного зрения)
// =====================================================
void drawStatusStripe(Card &c, uint16_t color) {
  // Перерисовываем только 4px слева - минимум SPI-трафика
  tft.fillRect(c.x, c.y + 1, 4, c.h - 2, color);
}

// =====================================================
// КЛИМАТ-КАРТОЧКА (без статусной полосы - информационные)
// =====================================================
void updateClimateCard(int id, Card &c, SensorValue &v, const char* fmt) {
  char buf[10];
  uint16_t col;
  if (v.ok) { snprintf(buf, sizeof(buf), fmt, v.value); col = COL_TEXT; }
  else      { snprintf(buf, sizeof(buf), "ERR");        col = COL_BAD;  }

  if (needsRedrawValue(id, buf, col)) {
    drawBigValue(c, 22, buf, col, 2);
  }
}

// =====================================================
// "ОПАСНАЯ" КАРТОЧКА (CO2/VOC/PM) - огромная цифра + цветная полоса
// =====================================================
void updateHealthCard(int id, Card &c, SensorValue &v, float warnT, float badT,
                      int valY, int textSize) {
  char buf[10];
  uint16_t col;
  if (v.ok) {
    snprintf(buf, sizeof(buf), "%.0f", v.value);
    col = thresholdColor(v.value, warnT, badT);
  } else {
    snprintf(buf, sizeof(buf), "ERR");
    col = COL_BAD;
  }

  // Полоса слева - отдельный кэш, перерисовывается только при смене цвета
  if (needsRedrawStripe(id, col)) {
    drawStatusStripe(c, col);
  }

  if (needsRedrawValue(id, buf, col)) {
    drawBigValue(c, valY, buf, col, textSize);
  }
}

// =====================================================
// ГЛАВНОЕ ОБНОВЛЕНИЕ ЭКРАНА
// =====================================================
void updateDisplay() {
  updateClimateCard(ID_TEMP, cTemp, vTemp, "%.1f");
  updateClimateCard(ID_HUM,  cHum,  vHum,  "%.1f");
  updateClimateCard(ID_PRES, cPres, vPres, "%.0f");

  // CO2/VOC - самые крупные (размер 5), т.к. это главные индикаторы качества воздуха
  updateHealthCard(ID_CO2, cCo2, vCo2, 1000.0f, 1500.0f, 22, 5);
  updateHealthCard(ID_VOC, cVoc, vVoc, 100.0f,  200.0f,  22, 5);

  // PM - размер 4 (чуть мельче, но всё ещё крупные)
  updateHealthCard(ID_PM1,  cPm1,  vPm1,  50.0f, 100.0f, 28, 4);
  updateHealthCard(ID_PM25, cPm25, vPm25, 35.0f, 75.0f,  28, 4);
  updateHealthCard(ID_PM10, cPm10, vPm10, 50.0f, 100.0f, 28, 4);
}
