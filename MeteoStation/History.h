#pragma once
#include <Arduino.h>
#include "Ui.h"

// Кольцевой буфер истории значений (храним в int16 с масштабом)
template <int N>
struct History {
  int16_t buf[N];
  int head = 0;
  int count = 0;

  void push(int16_t v) {
    buf[head] = v;
    head = (head + 1) % N;
    if (count < N) count++;
  }

  // 0 = самое старое значение
  int16_t at(int i) const { return buf[(head - count + i + N) % N]; }
};

// Заполняет график (усреднение по корзинам) и изменение за lag отсчётов.
// spanScale: если истории меньше lag, изменение пересчитывается на полное окно.
template <int N>
void fillTrend(Trend& t, const History<N>& h, float scale, int lag, int minSamples, bool spanScale = false) {
  int c = h.count;
  int n = min(c, SPARK_POINTS);
  t.count = n;
  for (int j = 0; j < n; j++) {
    int from = j * c / n, to = (j + 1) * c / n;
    if (to <= from) to = from + 1;
    float sum = 0;
    for (int i = from; i < to; i++) sum += h.at(i);
    t.points[j] = sum / (to - from) * scale;
  }
  t.valid = c >= minSamples;
  if (t.valid) {
    int back = min(lag, c - 1);
    t.delta = (h.at(c - 1) - h.at(c - 1 - back)) * scale;
    if (spanScale && back > 0 && back < lag) t.delta = t.delta * lag / back;
  }
}
