#include "Ui.h"
#include "Fonts.h"
#include <math.h>

using namespace gfx;

namespace {

// =====================================================
// ПАЛИТРА
// =====================================================
const uint32_t BG_TOP       = 0x11141C;
const uint32_t BG_BOTTOM    = 0x06070A;
const uint32_t PANEL_TOP    = 0x1B1F2A;
const uint32_t PANEL_BOTTOM = 0x12151C;
const uint16_t BORDER       = rgb(38, 44, 58);
const uint16_t TEXT         = rgb(242, 245, 250);
const uint16_t TEXT2        = rgb(146, 155, 174);
const uint16_t TEXT3        = rgb(88, 97, 117);
const uint16_t TRACK        = rgb(36, 41, 54);

// Уровни: 0 отлично, 1 хорошо, 2 средне, 3 плохо, 4 опасно
const uint16_t LEVEL[5] = {
  rgb(52, 211, 153), rgb(163, 230, 53), rgb(251, 191, 36), rgb(251, 146, 60), rgb(244, 63, 94)
};
const char* const LEVEL_NAME[5] = {"Отлично", "Хорошо", "Средне", "Плохо", "Опасно"};
// Градиент кольца: от 0 (красный) до 100 (зелёный)
const uint16_t RING_STOPS[5] = {LEVEL[4], LEVEL[3], LEVEL[2], LEVEL[1], LEVEL[0]};

const uint16_t C_TEMP = rgb(251, 146, 60);
const uint16_t C_HUM  = rgb(56, 189, 248);
const uint16_t C_PRES = rgb(167, 139, 250);

// =====================================================
// ПОРОГИ И ОЦЕНКА
// =====================================================
struct Pt { float v, s; };

// Точки "значение -> балл". Границы 80/60/40/20 совпадают с уровнями.
const Pt CO2_S[]  = {{400, 100}, {800, 80}, {1000, 60}, {1400, 40}, {2000, 20}, {3000, 0}};
const Pt VOC_S[]  = {{0, 100}, {100, 80}, {150, 60}, {250, 40}, {400, 20}, {500, 0}};
const Pt PM25_S[] = {{0, 100}, {12, 80}, {25, 60}, {35, 40}, {55, 20}, {150, 0}};
const Pt PM10_S[] = {{0, 100}, {20, 80}, {50, 60}, {100, 40}, {150, 20}, {300, 0}};
const Pt PM1_S[]  = {{0, 100}, {10, 80}, {20, 60}, {30, 40}, {50, 20}, {120, 0}};

template <int N>
float scoreOf(const Pt (&p)[N], float v) {
  if (v <= p[0].v) return p[0].s;
  for (int i = 1; i < N; i++)
    if (v <= p[i].v) return p[i - 1].s + (p[i].s - p[i - 1].s) * (v - p[i - 1].v) / (p[i].v - p[i - 1].v);
  return p[N - 1].s;
}

int levelOfScore(float s) {
  if (s >= 80) return 0;
  if (s >= 60) return 1;
  if (s >= 40) return 2;
  if (s >= 20) return 3;
  return 4;
}

template <int N>
int levelOf(const Pt (&p)[N], float v) { return levelOfScore(scoreOf(p, v) - 0.001f); }

// =====================================================
// АНИМАЦИЯ ЗНАЧЕНИЙ
// =====================================================
struct Anim {
  float v = NAN;
  // Возвращает true, пока значение ещё движется к цели
  bool step(float target, float k) {
    if (isnan(target)) { bool was = !isnan(v); v = NAN; return was; }
    if (isnan(v)) { v = target; return true; }
    if (v == target) return false;
    v += (target - v) * k;
    if (fabsf(target - v) < 0.05f) v = target;
    return true;
  }
};

Anim aScore, aCo2, aVoc, aPm1, aPm25, aPm10, aTemp, aHum, aPres;
uint32_t frameNow = 0;

// =====================================================
// ВСПОМОГАТЕЛЬНОЕ
// =====================================================
void panel(Canvas& cv, int x, int y, int w, int h, int r) {
  cv.fillPanel(x, y, w, h, r, BORDER, PANEL_TOP, PANEL_BOTTOM, 70);
}

// Подпись с нижним индексом для CO2
int label(Canvas& cv, int x, int y, const char* s, uint16_t c) {
  if (strcmp(s, "CO2") == 0) {
    int ex = cv.text(fontLabel, x, y, "CO", c);
    return cv.text(fontSmall, ex, y + 2, "2", c);
  }
  return cv.text(fontLabel, x, y, s, c);
}

enum Dir { DIR_FLAT, DIR_UP, DIR_DOWN };

void arrow(Canvas& cv, float x, float baseline, Dir dir, uint16_t c) {
  float top = baseline - 9, bot = baseline - 2;
  if (dir == DIR_UP) {
    float xs[] = {x + 3.5f, x + 7.5f, x - 0.5f};
    float ys[] = {top, bot, bot};
    cv.fillPolygon(xs, ys, 3, c);
  } else if (dir == DIR_DOWN) {
    float xs[] = {x - 0.5f, x + 7.5f, x + 3.5f};
    float ys[] = {top, top, bot};
    cv.fillPolygon(xs, ys, 3, c);
  } else {
    cv.fillRoundRect(x, baseline - 6.5f, 7, 2.2f, 1.1f, c);
  }
}

// Тренд в правом верхнем углу карточки: стрелка + изменение
void trendBadge(Canvas& cv, int right, int baseline, const Trend& t, float thr, const char* fmt,
                bool upIsBad) {
  if (!t.valid) return;
  Dir dir = t.delta >= thr ? DIR_UP : (t.delta <= -thr ? DIR_DOWN : DIR_FLAT);
  uint16_t c = TEXT3;
  if (dir != DIR_FLAT) c = ((dir == DIR_UP) == upIsBad) ? LEVEL[3] : LEVEL[0];
  char buf[16];
  snprintf(buf, sizeof(buf), fmt, t.delta);
  if (dir == DIR_FLAT) snprintf(buf, sizeof(buf), "%s", "ровно");
  int tw = cv.textWidth(fontSmall, buf);
  cv.text(fontSmall, right, baseline, buf, dir == DIR_FLAT ? TEXT3 : TEXT2, RIGHT);
  arrow(cv, right - tw - 10, baseline, dir, c);
}

void stateText(Canvas& cv, int x, int baseline, SensorState s, Align al = LEFT) {
  if (s == S_WARMUP) {
    // Бегущие точки, чтобы было видно, что идёт прогрев
    int dots = (frameNow / 400) % 4;
    char buf[20] = "прогрев";
    for (int i = 0; i < dots; i++) strcat(buf, ".");
    if (al == LEFT) cv.text(fontLabel, x, baseline, buf, TEXT2);
    else cv.text(fontLabel, x - cv.textWidth(fontLabel, "прогрев") / 2, baseline, buf, TEXT2);
  } else {
    cv.text(fontLabel, x, baseline, "нет данных", LEVEL[4], al);
  }
}

// =====================================================
// ИНДЕКС ВОЗДУХА (кольцо)
// =====================================================
const char* const FACTOR_NAME[4] = {"CO2", "VOC", "PM2.5", "PM10"};

void drawScore(Canvas& cv, const UiData& d, int x, int y, int w, int h) {
  panel(cv, x, y, w, h, 12);
  const float A0 = 135, A1 = 405, R = 48, T = 9;
  float cx = x + w * 0.5f, cy = y + 57;

  // Таблица углов для кольца строится один раз (центр не двигается)
  static PolarCache ring;
  static bool ringTried = false;
  if (!ringTried) { ring.build(cx, cy, R + 9); ringTried = true; }

  cv.arc(ring, R, T, A0, A1, RING_STOPS, 5, A0, A1, 40);

  int worst = -1;
  float target = airScore(d, &worst);
  if (isnan(aScore.v) || isnan(target)) {
    cv.text(fontSmall, cx, cy - 23, "ИНДЕКС", TEXT3, CENTER);
    cv.text(fontHero, cx, cy + 15, "--", TEXT3, CENTER);
    cv.text(fontLabel, cx, y + h - 8, "Ожидание", TEXT3, CENTER);
    return;
  }

  float s = aScore.v;
  int lvl = levelOfScore(target);
  uint16_t lc = LEVEL[lvl];
  float aEnd = A0 + 270.0f * s / 100.0f;

  // Мягкое свечение; при плохом воздухе оно медленно пульсирует
  float pulse = lvl >= 3 ? 0.5f + 0.5f * sinf(frameNow * 0.004f) : 1.0f;
  cv.arc(ring, R + 4, T + 8, A0, aEnd, RING_STOPS, 5, A0, A1, (uint8_t)(30 + 28 * pulse), 4.0f);
  cv.arc(ring, R, T, A0, aEnd, RING_STOPS, 5, A0, A1);

  // Бегунок на конце дуги
  const float D2R = (float)M_PI / 180.0f;
  float rm = R - T * 0.5f;
  float kx = cx + rm * cosf(aEnd * D2R), ky = cy + rm * sinf(aEnd * D2R);
  cv.fillCircle(kx, ky, 7, lc, 60, 3);
  cv.fillCircle(kx, ky, 3.4f, TEXT);

  char buf[8];
  snprintf(buf, sizeof(buf), "%d", (int)lroundf(s));
  cv.text(fontSmall, cx, cy - 23, "ИНДЕКС", TEXT3, CENTER);
  cv.text(fontHero, cx, cy + 15, buf, TEXT, CENTER);
  if (lvl > 0 && worst >= 0) {
    char why[24];
    snprintf(why, sizeof(why), "из-за %s", FACTOR_NAME[worst]);
    cv.text(fontSmall, cx, cy + 30, why, TEXT2, CENTER);
  } else {
    cv.text(fontSmall, cx, cy + 30, "из 100", TEXT3, CENTER);
  }
  cv.text(fontLabel, cx, y + h - 8, LEVEL_NAME[lvl], lc, CENTER);
}

// =====================================================
// КАРТОЧКА С ГРАФИКОМ (CO2, VOC)
// =====================================================
template <int N>
void drawChartCard(Canvas& cv, int x, int y, int w, int h, const char* name, const char* unit,
                   const Metric& m, const Anim& a, const Trend& t, const Pt (&scale)[N],
                   float minSpan, float trendThr) {
  panel(cv, x, y, w, h, 10);
  int lx = x + 12, row1 = y + 17;
  int ex = label(cv, lx, row1, name, TEXT2);
  cv.text(fontSmall, ex + 5, row1, unit, TEXT3);
  trendBadge(cv, x + w - 12, row1, t, trendThr, "%+.0f", true);

  int gx0 = x + 96, gx1 = x + w - 12;
  uint16_t col = TEXT2;
  if (m.ok() && !isnan(a.v)) {
    col = LEVEL[levelOf(scale, m.value)];
    char buf[8];
    snprintf(buf, sizeof(buf), "%d", (int)lroundf(a.v));
    int valueRight = cv.text(fontBig, lx - 1, y + h - 6, buf, col);
    gx0 = max(gx0, valueRight + 6);
  } else {
    stateText(cv, lx, y + h - 13, m.state);
  }

  // График за последний час
  if (gx0 >= gx1) return;
  int gy0 = y + 26, gy1 = y + h - 8;
  for (int gx = gx0; gx <= gx1; gx += 3) cv.fillRect(gx, gy1, 1, 1, TEXT3, 120);
  if (t.count < 2) return;

  float lo = t.points[0], hi = t.points[0];
  for (int i = 1; i < t.count; i++) { lo = min(lo, t.points[i]); hi = max(hi, t.points[i]); }
  if (hi - lo < minSpan) { float c = (hi + lo) * 0.5f; lo = c - minSpan * 0.5f; hi = c + minSpan * 0.5f; }

  float xs[SPARK_POINTS], ys[SPARK_POINTS];
  int n = t.count;
  for (int i = 0; i < n; i++) {
    xs[i] = gx0 + (gx1 - gx0) * (float)i / (n - 1);
    ys[i] = gy1 - 1 - (gy1 - gy0 - 2) * (t.points[i] - lo) / (hi - lo);
  }
  cv.sparkline(xs, ys, n, 1.7f, col, gy1, 80);
  cv.fillCircle(xs[n - 1], ys[n - 1], 5.5f, col, 70, 2.5f);
  cv.fillCircle(xs[n - 1], ys[n - 1], 2.4f, TEXT);
}

// =====================================================
// ПЫЛЬ
// =====================================================
template <int N>
void drawPm(Canvas& cv, int x, int y, int w, int h, const char* name, const Metric& m,
            const Anim& a, const Pt (&scale)[N], float barMax) {
  panel(cv, x, y, w, h, 10);
  cv.text(fontLabel, x + 11, y + 18, name, TEXT2);

  int bx = x + 11, bw = w - 22, by = y + h - 9, bh = 3;
  // Шкала уровней (приглушённая)
  for (int i = 0; i < bw; i++) {
    float v = barMax * (i + 0.5f) / bw;
    cv.fillRect(bx + i, by, 1, bh, LEVEL[levelOf(scale, v)], 55);
  }

  if (!m.ok() || isnan(a.v)) {
    stateText(cv, x + 11, y + 38, m.state);
    return;
  }
  uint16_t col = LEVEL[levelOf(scale, m.value)];
  char buf[8];
  snprintf(buf, sizeof(buf), "%d", (int)lroundf(a.v));
  const char* unit = "мкг/м³";
  int available = w - 16 - 4 - cv.textWidth(fontSmall, unit);
  const Font& valueFont = cv.textWidth(fontMed, buf) <= available ? fontMed : fontLabel;
  int ex = cv.text(valueFont, x + 10, y + 41, buf, col);
  cv.text(fontSmall, ex + 4, y + 41, unit, TEXT3);

  int fill = (int)lroundf(bw * min(1.0f, max(0.0f, a.v / barMax)));
  for (int i = 0; i < fill; i++) {
    float v = barMax * (i + 0.5f) / bw;
    cv.fillRect(bx + i, by, 1, bh, LEVEL[levelOf(scale, v)]);
  }
  cv.fillCircle(bx + fill, by + bh * 0.5f, 4.5f, col, 80, 2);
  cv.fillCircle(bx + fill, by + bh * 0.5f, 2.6f, TEXT);
}

// =====================================================
// КЛИМАТ
// =====================================================
enum Icon { ICON_TEMP, ICON_HUM, ICON_PRES };

void drawIcon(Canvas& cv, Icon icon, float cx, float cy, uint16_t c) {
  if (icon == ICON_TEMP) {
    cv.fillRoundRect(cx - 3.5f, cy - 11, 7, 16, 3.5f, c, 60);
    cv.fillCircle(cx, cy + 6, 5.5f, c);
    cv.fillRoundRect(cx - 1.4f, cy - 7, 2.8f, 12, 1.4f, c);
  } else if (icon == ICON_HUM) {
    float xs[] = {cx, cx + 5.6f, cx - 5.6f};
    float ys[] = {cy - 10, cy + 0.5f, cy + 0.5f};
    cv.fillPolygon(xs, ys, 3, c);
    cv.fillCircle(cx, cy + 3, 6.5f, c);
    cv.fillCircle(cx - 2.2f, cy + 3.5f, 1.8f, TEXT, 120);
  } else {
    uint16_t st[1] = {c};
    cv.arc(cx, cy + 2, 9.5f, 2.6f, 150, 390, st, 1, 0, 1);
    const float D2R = (float)M_PI / 180.0f;
    float na = 300 * D2R;
    cv.line(cx, cy + 2, cx + 6 * cosf(na), cy + 2 + 6 * sinf(na), 2.0f, TEXT);
    cv.fillCircle(cx, cy + 2, 2.2f, c);
  }
}

void drawClimate(Canvas& cv, int x, int y, int w, int h, Icon icon, uint16_t accent,
                 const Metric& m, const char* value, const char* unit,
                 const char* descr, uint16_t descrColor) {
  panel(cv, x, y, w, h, 10);
  if (!m.ok()) {
    cv.text(fontSmall, x + 11, y + 17, descr, TEXT3);
    stateText(cv, x + 11, y + h - 10, m.state);
    return;
  }
  drawIcon(cv, icon, x + 16, y + h * 0.5f, accent);
  int tx = x + 31;
  cv.text(fontSmall, tx, y + 17, descr, descrColor);
  int ex = cv.text(fontMed, tx - 1, y + h - 6, value, TEXT);
  if (unit) cv.text(fontSmall, ex + 3, y + h - 6, unit, TEXT3);
}

}  // namespace

// =====================================================
// ПУБЛИЧНОЕ
// =====================================================
float airScore(const UiData& d, int* worstIndex) {
  float best = NAN;
  int worst = -1;
  const Metric* ms[4] = {&d.co2, &d.voc, &d.pm25, &d.pm10};
  for (int i = 0; i < 4; i++) {
    if (!ms[i]->ok()) continue;
    float v = ms[i]->value, s;
    switch (i) {
      case 0: s = scoreOf(CO2_S, v); break;
      case 1: s = scoreOf(VOC_S, v); break;
      case 2: s = scoreOf(PM25_S, v); break;
      default: s = scoreOf(PM10_S, v); break;
    }
    if (isnan(best) || s < best) { best = s; worst = i; }
  }
  if (worstIndex) *worstIndex = worst;
  return best;
}

// =====================================================
// РАСКЛАДКА ЭКРАНА 320x240
// =====================================================
struct Box { int16_t x, y, w, h, r; };

static const Box B_SCORE = {6, 6, 120, 116, 12};
static const Box B_CO2   = {132, 6, 182, 55, 10};
static const Box B_VOC   = {132, 67, 182, 55, 10};
static const Box B_PM1   = {6, 128, 98, 54, 10};
static const Box B_PM25  = {110, 128, 98, 54, 10};
static const Box B_PM10  = {214, 128, 100, 54, 10};
static const Box B_TEMP  = {6, 188, 98, 46, 10};
static const Box B_HUM   = {110, 188, 98, 46, 10};
static const Box B_PRES  = {214, 188, 100, 46, 10};
static const Box* const BOXES[] = {&B_SCORE, &B_CO2, &B_VOC, &B_PM1, &B_PM25, &B_PM10, &B_TEMP, &B_HUM, &B_PRES};
static const int BOX_COUNT = sizeof(BOXES) / sizeof(BOXES[0]);

// =====================================================
// ОБНОВЛЕНИЕ: анимации и признак "нужно перерисовать"
// =====================================================
static uint32_t sigLast = 0;

static inline void sigAdd(uint32_t& h, int32_t v) {
  h ^= (uint32_t)v;
  h *= 16777619u;
}

static void sigMetric(uint32_t& h, const Metric& m, float scale) {
  sigAdd(h, m.state);
  if (m.ok()) sigAdd(h, (int32_t)lroundf(m.value * scale));
}

static void sigTrend(uint32_t& h, const Trend& t) {
  sigAdd(h, t.count);
  sigAdd(h, t.valid);
  sigAdd(h, (int32_t)lroundf(t.delta * 10));
  if (t.count) sigAdd(h, (int32_t)lroundf(t.points[t.count - 1] * 10));
}

bool uiUpdate(const UiData& d, uint32_t now) {
  frameNow = now;
  const float K = 0.22f;
  float score = airScore(d, nullptr);
  // Индекс при первом появлении "набегает" с нуля
  if (isnan(aScore.v) && !isnan(score)) aScore.v = 0;
  bool moving = false;
  moving |= aScore.step(score, 0.12f);
  moving |= aCo2.step(d.co2.ok() ? d.co2.value : NAN, K);
  moving |= aVoc.step(d.voc.ok() ? d.voc.value : NAN, K);
  moving |= aPm1.step(d.pm1.ok() ? d.pm1.value : NAN, K);
  moving |= aPm25.step(d.pm25.ok() ? d.pm25.value : NAN, K);
  moving |= aPm10.step(d.pm10.ok() ? d.pm10.value : NAN, K);
  moving |= aTemp.step(d.temp.ok() ? d.temp.value : NAN, K);
  moving |= aHum.step(d.hum.ok() ? d.hum.value : NAN, K);
  moving |= aPres.step(d.pres.ok() ? d.pres.value : NAN, K);

  // Подпись того, что видно на экране: если не изменилась, кадр не нужен
  uint32_t h = 2166136261u;
  const Metric* ms[] = {&d.temp, &d.hum, &d.pres, &d.co2, &d.voc, &d.pm1, &d.pm25, &d.pm10};
  bool warmup = false;
  for (const Metric* m : ms) {
    sigMetric(h, *m, 10);
    warmup |= m->state == S_WARMUP;
  }
  sigTrend(h, d.co2Trend);
  sigTrend(h, d.vocTrend);
  sigTrend(h, d.presTrend);
  if (warmup) sigAdd(h, now / 400);  // бегущие точки "прогрев..."

  bool changed = h != sigLast;
  sigLast = h;
  // Плохой воздух: кольцо пульсирует, рисуем непрерывно
  bool pulse = !isnan(score) && levelOfScore(score) >= 3;
  return moving || changed || pulse;
}

// =====================================================
// КАДР
// =====================================================
void uiDraw(Canvas& cv, const UiData& d, uint32_t now) {
  (void)now;
  // Фон не рисуем там, где его гарантированно закроют карточки
  gfx::Rect opaque[BOX_COUNT];
  for (int i = 0; i < BOX_COUNT; i++) {
    const Box& b = *BOXES[i];
    opaque[i] = {b.x, (int16_t)(b.y + b.r), b.w, (int16_t)(b.h - 2 * b.r)};
  }
  cv.fillBackground(BG_TOP, BG_BOTTOM, opaque, BOX_COUNT);

  // Верх: индекс + две карточки с графиками
  drawScore(cv, d, B_SCORE.x, B_SCORE.y, B_SCORE.w, B_SCORE.h);
  drawChartCard(cv, B_CO2.x, B_CO2.y, B_CO2.w, B_CO2.h, "CO2", "ppm", d.co2, aCo2, d.co2Trend, CO2_S, 120, 25);
  drawChartCard(cv, B_VOC.x, B_VOC.y, B_VOC.w, B_VOC.h, "VOC", "индекс", d.voc, aVoc, d.vocTrend, VOC_S, 40, 12);

  // Середина: пыль
  drawPm(cv, B_PM1.x, B_PM1.y, B_PM1.w, B_PM1.h, "PM1.0", d.pm1, aPm1, PM1_S, 60);
  drawPm(cv, B_PM25.x, B_PM25.y, B_PM25.w, B_PM25.h, "PM2.5", d.pm25, aPm25, PM25_S, 75);
  drawPm(cv, B_PM10.x, B_PM10.y, B_PM10.w, B_PM10.h, "PM10", d.pm10, aPm10, PM10_S, 180);

  // Низ: климат
  char buf[12];

  const char* tDescr = "Температура";
  uint16_t tCol = TEXT2;
  if (d.temp.ok()) {
    float v = d.temp.value;
    if (v < 18)      { tDescr = "Холодно";   tCol = C_HUM; }
    else if (v < 20) { tDescr = "Прохладно"; tCol = C_HUM; }
    else if (v <= 24){ tDescr = "Комфорт";   tCol = LEVEL[0]; }
    else if (v <= 27){ tDescr = "Тепло";     tCol = LEVEL[2]; }
    else             { tDescr = "Жарко";     tCol = LEVEL[4]; }
  }
  snprintf(buf, sizeof(buf), "%.1f°", isnan(aTemp.v) ? 0.0f : aTemp.v);
  drawClimate(cv, B_TEMP.x, B_TEMP.y, B_TEMP.w, B_TEMP.h, ICON_TEMP, C_TEMP, d.temp, buf, nullptr, tDescr, tCol);

  const char* hDescr = "Влажность";
  uint16_t hCol = TEXT2;
  if (d.hum.ok()) {
    float v = d.hum.value;
    if (v < 30)       { hDescr = "Сухо";    hCol = LEVEL[2]; }
    else if (v <= 60) { hDescr = "Комфорт"; hCol = LEVEL[0]; }
    else if (v <= 70) { hDescr = "Влажно";  hCol = C_HUM; }
    else              { hDescr = "Сыро";    hCol = LEVEL[3]; }
  }
  snprintf(buf, sizeof(buf), "%.0f%%", isnan(aHum.v) ? 0.0f : aHum.v);
  drawClimate(cv, B_HUM.x, B_HUM.y, B_HUM.w, B_HUM.h, ICON_HUM, C_HUM, d.hum, buf, nullptr, hDescr, hCol);

  const char* pDescr = "Давление";
  uint16_t pCol = TEXT2;
  if (d.pres.ok()) {
    if (d.presTrend.valid && d.presTrend.delta >= 1.5f)       { pDescr = "Растёт";  pCol = LEVEL[0]; }
    else if (d.presTrend.valid && d.presTrend.delta <= -1.5f) { pDescr = "Падает";  pCol = LEVEL[2]; }
    else if (d.pres.value < 740)                              { pDescr = "Низкое";  pCol = LEVEL[2]; }
    else if (d.pres.value > 770)                              { pDescr = "Высокое"; pCol = TEXT2; }
    else                                                      { pDescr = "Стабильно"; pCol = TEXT2; }
  }
  snprintf(buf, sizeof(buf), "%.0f", isnan(aPres.v) ? 0.0f : aPres.v);
  drawClimate(cv, B_PRES.x, B_PRES.y, B_PRES.w, B_PRES.h, ICON_PRES, C_PRES, d.pres, buf, "мм", pDescr, pCol);
}