#pragma once
#include "Gfx.h"

// =====================================================
// Данные для экрана. Прошивка заполняет UiData, экран только рисует.
// =====================================================

enum SensorState : uint8_t {
  S_OK,       // свежие данные
  S_WARMUP,   // датчик ещё прогревается
  S_LOST      // данных нет или они устарели
};

struct Metric {
  float value = NAN;
  SensorState state = S_WARMUP;
  bool ok() const { return state == S_OK; }
};

constexpr int SPARK_POINTS = 48;

struct Trend {
  float points[SPARK_POINTS];  // для графика, старые слева
  int count = 0;
  float delta = 0;             // изменение за окно тренда
  bool valid = false;
};

struct UiData {
  Metric temp, hum, pres, co2, voc, pm1, pm25, pm10;
  Trend co2Trend, vocTrend;
  Trend presTrend;             // изменение давления за 3 часа
};

// Индекс качества воздуха 0..100 (100 = идеально), NAN если данных нет
float airScore(const UiData& d, int* worstIndex);

// Анимации считаются один раз за кадр, рисование может идти полосами.
// Возвращает true, если картинка изменилась и кадр стоит перерисовать.
bool uiUpdate(const UiData& d, uint32_t now);
void uiDraw(gfx::Canvas& cv, const UiData& d, uint32_t now);
