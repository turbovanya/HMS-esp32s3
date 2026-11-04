#pragma once
#include <Arduino.h>

// =====================================================
// Мини-рендер со сглаживанием поверх буфера RGB565.
// Кадр рисуется целиком в память и одним куском уходит в дисплей,
// поэтому мерцания нет. Если памяти на весь кадр не хватает,
// кадр рисуется полосами (band): сцена прогоняется для каждой полосы.
// =====================================================

struct Glyph {
  uint32_t offset;
  uint8_t w, h;
  int8_t xo, yo;
  uint8_t adv;
};

struct Font {
  const uint8_t* bmp;
  const uint16_t* codes;
  const Glyph* glyphs;
  uint16_t count;
  uint8_t ascent, descent, capH;
};

namespace gfx {

constexpr uint16_t rgb(uint8_t r, uint8_t g, uint8_t b) {
  return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}

// t = 0 -> a, t = 255 -> b
uint16_t mix(uint16_t a, uint16_t b, uint8_t t);

enum Align { LEFT, CENTER, RIGHT };

struct Rect { int16_t x, y, w, h; };

// Таблица угла и расстояния для каждого пикселя вокруг неподвижного центра.
// Считается один раз, после чего дуги рисуются без корней и арктангенсов.
class PolarCache {
public:
  bool build(float cx, float cy, float rMax);
  bool ready() const { return ang_ != nullptr; }

private:
  friend class Canvas;
  float cx_ = 0, cy_ = 0;
  int x0_ = 0, y0_ = 0, size_ = 0;
  uint16_t* ang_ = nullptr;   // угол в сотых долях градуса, 0..35999
  uint16_t* dist_ = nullptr;  // расстояние в 1/16 пикселя
};

class Canvas {
public:
  bool begin(int w, int h);

  int width() const { return w_; }
  int height() const { return h_; }
  int bandHeight() const { return bandH_; }
  int bandCount() const { return (h_ + bandH_ - 1) / bandH_; }
  void setBand(int index);
  int bandY() const { return bandY_; }
  int bandRows() const { return rows_; }
  uint16_t* buffer() { return buf_; }

  void blend(int x, int y, uint16_t c, uint8_t a);

  // Фон с вертикальным градиентом и упорядоченным дизерингом.
  // skip: области, которые всё равно будут закрашены, их пропускаем.
  void fillBackground(uint32_t top, uint32_t bottom, const Rect* skip = nullptr, int nSkip = 0);
  void fillRect(int x, int y, int w, int h, uint16_t c, uint8_t a = 255);
  void fillRoundRect(float x, float y, float w, float h, float r, uint16_t c, uint8_t a = 255);
  // Карточка: обводка 1px, градиентная заливка, блик по верхней кромке. Один проход.
  void fillPanel(int x, int y, int w, int h, int r, uint16_t border, uint32_t top, uint32_t bottom,
                 uint8_t hilite);
  void fillCircle(float cx, float cy, float r, uint16_t c, uint8_t a = 255, float feather = 1.0f);

  // Дуга с круглыми концами. Углы в градусах: 0 = вправо, 90 = вниз.
  // stops задают градиент вдоль угла от gA0 до gA1.
  void arc(float cx, float cy, float rOut, float thick, float a0, float a1,
           const uint16_t* stops, int nStops, float gA0, float gA1,
           uint8_t alpha = 255, float feather = 1.0f);
  // То же самое, но через готовую таблицу (центр берётся из неё)
  void arc(const PolarCache& pc, float rOut, float thick, float a0, float a1,
           const uint16_t* stops, int nStops, float gA0, float gA1,
           uint8_t alpha = 255, float feather = 1.0f);

  void line(float x0, float y0, float x1, float y1, float w, uint16_t c, uint8_t a = 255);
  // Выпуклый многоугольник (вершины по часовой стрелке в экранных координатах)
  void fillPolygon(const float* xs, const float* ys, int n, uint16_t c, uint8_t a = 255);

  // Ломаная для графиков: xs по возрастанию. Под линией заливка с затуханием.
  void sparkline(const float* xs, const float* ys, int n, float w, uint16_t c,
                 float fillBottom, uint8_t fillAlpha);

  int text(const Font& f, int x, int y, const char* s, uint16_t c, Align al = LEFT, uint8_t a = 255);
  int textWidth(const Font& f, const char* s);

private:
  uint16_t* buf_ = nullptr;
  int w_ = 0, h_ = 0;
  int bandH_ = 0, bandY_ = 0, rows_ = 0;

  inline bool inBand(int y) const { return y >= bandY_ && y < bandY_ + rows_; }
  void clipY(int& y0, int& y1) const;
  const Glyph* findGlyph(const Font& f, uint16_t code) const;
};

uint16_t gradientAt(const uint16_t* stops, int n, float t);

}  // namespace gfx
