// Рисование попиксельное, поэтому здесь важна скорость, а не размер кода
#pragma GCC optimize("O2")

#include "Gfx.h"
#include <math.h>

namespace gfx {

static const uint8_t BAYER[4][4] = {
  { 0,  8,  2, 10},
  {12,  4, 14,  6},
  { 3, 11,  1,  9},
  {15,  7, 13,  5},
};

static inline float clamp01(float v) { return v < 0 ? 0 : (v > 1 ? 1 : v); }

static inline uint16_t dither565(int r, int g, int b, int x, int y) {
  int d = BAYER[y & 3][x & 3];
  r += d >> 1; g += d >> 2; b += d >> 1;
  if (r > 255) r = 255;
  if (g > 255) g = 255;
  if (b > 255) b = 255;
  return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}

uint16_t mix(uint16_t a, uint16_t b, uint8_t t) {
  uint32_t rb = a & 0xF81F;
  rb += ((b & 0xF81F) - rb) * (t >> 2) >> 6;
  uint32_t g = a & 0x07E0;
  g += ((b & 0x07E0) - g) * t >> 8;
  return (rb & 0xF81F) | (g & 0x07E0);
}

uint16_t gradientAt(const uint16_t* stops, int n, float t) {
  if (n <= 1) return stops[0];
  t = clamp01(t) * (n - 1);
  int i = (int)t;
  if (i >= n - 1) return stops[n - 1];
  return mix(stops[i], stops[i + 1], (uint8_t)((t - i) * 255));
}

// =====================================================
// БУФЕР И ПОЛОСЫ
// =====================================================
bool Canvas::begin(int w, int h) {
  w_ = w; h_ = h;
  // Сначала пробуем весь кадр, потом всё меньшие полосы
  static const int bands[] = {240, 120, 80, 60, 40, 24};
  for (int bh : bands) {
    if (bh > h) continue;
    buf_ = (uint16_t*)heap_caps_malloc((size_t)w * bh * 2, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (buf_) { bandH_ = bh; setBand(0); return true; }
  }
  return false;
}

void Canvas::setBand(int index) {
  bandY_ = index * bandH_;
  rows_ = min(bandH_, h_ - bandY_);
}

void Canvas::clipY(int& y0, int& y1) const {
  if (y0 < bandY_) y0 = bandY_;
  if (y1 > bandY_ + rows_) y1 = bandY_ + rows_;
}

__attribute__((always_inline)) inline void Canvas::blend(int x, int y, uint16_t c, uint8_t a) {
  if (!a || x < 0 || x >= w_ || !inBand(y)) return;
  uint16_t* p = buf_ + (y - bandY_) * w_ + x;
  *p = (a == 255) ? c : mix(*p, c, a);
}

// =====================================================
// ЗАЛИВКИ
// =====================================================
// Дизеринг повторяется каждые 4 пикселя, поэтому строку заливаем готовым шаблоном
static inline void rowPattern(uint16_t* pat, int r, int g, int b, int y) {
  for (int i = 0; i < 4; i++) pat[i] = dither565(r, g, b, i, y);
}

static inline void fillPattern(uint16_t* p, int x0, int x1, const uint16_t* pat) {
  for (int x = x0; x < x1; x++) p[x] = pat[x & 3];
}

void Canvas::fillBackground(uint32_t top, uint32_t bottom, const Rect* skip, int nSkip) {
  int r0 = top >> 16, g0 = (top >> 8) & 0xFF, b0 = top & 0xFF;
  int r1 = bottom >> 16, g1 = (bottom >> 8) & 0xFF, b1 = bottom & 0xFF;
  int gaps[16][2];
  for (int y = bandY_; y < bandY_ + rows_; y++) {
    uint16_t pat[4];
    rowPattern(pat, r0 + (r1 - r0) * y / (h_ - 1), g0 + (g1 - g0) * y / (h_ - 1),
               b0 + (b1 - b0) * y / (h_ - 1), y);
    uint16_t* p = buf_ + (y - bandY_) * w_;

    // Пропускаемые отрезки этой строки, по возрастанию x
    int n = 0;
    for (int i = 0; i < nSkip && n < 16; i++) {
      const Rect& s = skip[i];
      if (y < s.y || y >= s.y + s.h) continue;
      int j = n++;
      while (j > 0 && gaps[j - 1][0] > s.x) { gaps[j][0] = gaps[j - 1][0]; gaps[j][1] = gaps[j - 1][1]; j--; }
      gaps[j][0] = s.x; gaps[j][1] = s.x + s.w;
    }
    int x = 0;
    for (int i = 0; i < n; i++) {
      if (gaps[i][0] > x) fillPattern(p, x, min(w_, (int)gaps[i][0]), pat);
      x = max(x, gaps[i][1]);
    }
    if (x < w_) fillPattern(p, x, w_, pat);
  }
}
void Canvas::fillRect(int x, int y, int w, int h, uint16_t c, uint8_t a) {
  int x0 = max(0, x), x1 = min(w_, x + w);
  int y0 = y, y1 = y + h;
  clipY(y0, y1);
  for (int yy = y0; yy < y1; yy++) {
    uint16_t* p = buf_ + (yy - bandY_) * w_;
    for (int xx = x0; xx < x1; xx++) p[xx] = (a == 255) ? c : mix(p[xx], c, a);
  }
}

// Знаковое расстояние до скруглённого прямоугольника (отрицательное внутри)
static inline float roundRectSdf(float px, float py, float cx, float cy, float hw, float hh, float r) {
  float qx = fabsf(px - cx) - (hw - r);
  float qy = fabsf(py - cy) - (hh - r);
  if (qx <= 0 && qy <= 0) return (qx > qy ? qx : qy) - r;
  float ox = qx > 0 ? qx : 0, oy = qy > 0 ? qy : 0;
  return sqrtf(ox * ox + oy * oy) - r;
}

void Canvas::fillRoundRect(float x, float y, float w, float h, float r, uint16_t c, uint8_t a) {
  float cx = x + w * 0.5f, cy = y + h * 0.5f, hw = w * 0.5f, hh = h * 0.5f;
  int x0 = max(0, (int)floorf(x)), x1 = min(w_, (int)ceilf(x + w));
  int y0 = (int)floorf(y), y1 = (int)ceilf(y + h);
  clipY(y0, y1);
  for (int yy = y0; yy < y1; yy++) {
    uint16_t* p = buf_ + (yy - bandY_) * w_;
    for (int xx = x0; xx < x1; xx++) {
      float d = roundRectSdf(xx + 0.5f, yy + 0.5f, cx, cy, hw, hh, r);
      float cov = clamp01(0.5f - d);
      if (cov <= 0) continue;
      uint8_t aa = (uint8_t)(cov * a);
      p[xx] = (aa == 255) ? c : mix(p[xx], c, aa);
    }
  }
}

// Маски скруглённого угла (обводка и заливка), считаются один раз на радиус
struct CornerMask {
  int r = 0;
  uint8_t outer[16 * 16];
  uint8_t inner[16 * 16];
};

static const CornerMask& cornerMask(int r) {
  static CornerMask cache[3];
  for (auto& m : cache) if (m.r == r) return m;
  CornerMask* m = &cache[0];
  for (auto& c : cache) if (c.r == 0) { m = &c; break; }
  m->r = r;
  for (int j = 0; j < r; j++)
    for (int i = 0; i < r; i++) {
      float dx = r - i - 0.5f, dy = r - j - 0.5f;
      float d = sqrtf(dx * dx + dy * dy);
      m->outer[j * r + i] = (uint8_t)(clamp01(r - d + 0.5f) * 255);
      m->inner[j * r + i] = (uint8_t)(clamp01(r - 1 - d + 0.5f) * 255);
    }
  return *m;
}

void Canvas::fillPanel(int x, int y, int w, int h, int r, uint16_t border, uint32_t top, uint32_t bottom,
                       uint8_t hilite) {
  if (r > 16) r = 16;
  const CornerMask& cm = cornerMask(r);
  int r0 = top >> 16, g0 = (top >> 8) & 0xFF, b0 = top & 0xFF;
  int r1 = bottom >> 16, g1 = (bottom >> 8) & 0xFF, b1 = bottom & 0xFF;
  uint16_t hiliteColor = mix(border, 0xFFFF, 60);
  int xa = max(0, x), xb = min(w_, x + w);
  int y0 = y, y1 = y + h;
  clipY(y0, y1);
  for (int yy = y0; yy < y1; yy++) {
    int k = yy - y, den = max(1, h - 1);
    uint16_t pat[4];
    rowPattern(pat, r0 + (r1 - r0) * k / den, g0 + (g1 - g0) * k / den, b0 + (b1 - b0) * k / den, yy);
    if (k == 1 && hilite)
      for (int i = 0; i < 4; i++) pat[i] = mix(pat[i], hiliteColor, hilite);
    uint16_t* p = buf_ + (yy - bandY_) * w_;
    bool edgeRow = k == 0 || k == h - 1;

    if (k < r || k >= h - r) {
      // Строка со скруглёнными углами
      int j = k < r ? k : h - 1 - k;
      for (int xx = xa; xx < xb; xx++) {
        int kx = xx - x;
        if (kx < r || kx >= w - r) {
          int i = kx < r ? kx : w - 1 - kx;
          uint8_t co = cm.outer[j * r + i];
          if (!co) continue;
          uint16_t c = mix(border, pat[xx & 3], cm.inner[j * r + i]);
          p[xx] = (co == 255) ? c : mix(p[xx], c, co);
        } else {
          p[xx] = edgeRow ? border : pat[xx & 3];
        }
      }
    } else {
      if (x >= 0 && x < w_) p[x] = border;
      if (x + w - 1 >= 0 && x + w - 1 < w_) p[x + w - 1] = border;
      fillPattern(p, max(xa, x + 1), min(xb, x + w - 1), pat);
    }
  }
}
void Canvas::fillCircle(float cx, float cy, float r, uint16_t c, uint8_t a, float feather) {
  float ext = r + feather;
  int x0 = max(0, (int)floorf(cx - ext)), x1 = min(w_, (int)ceilf(cx + ext));
  int y0 = (int)floorf(cy - ext), y1 = (int)ceilf(cy + ext);
  clipY(y0, y1);
  for (int yy = y0; yy < y1; yy++) {
    float dy = yy + 0.5f - cy;
    for (int xx = x0; xx < x1; xx++) {
      float dx = xx + 0.5f - cx;
      float d = sqrtf(dx * dx + dy * dy);
      float cov = clamp01((r - d) / feather + 0.5f);
      if (cov > 0) blend(xx, yy, c, (uint8_t)(cov * a));
    }
  }
}

// =====================================================
// ДУГА
// =====================================================
// Быстрый atan2 в градусах (погрешность меньше 0.01 градуса)
static inline float fastAtan2Deg(float y, float x) {
  float ax = fabsf(x), ay = fabsf(y);
  float mn = ax < ay ? ax : ay, mx = ax < ay ? ay : ax;
  if (mx == 0) return 0;
  float a = mn / mx, s = a * a;
  float r = ((-0.0464964749f * s + 0.15931422f) * s - 0.327622764f) * s * a + a;
  if (ay > ax) r = 1.57079637f - r;
  if (x < 0) r = 3.14159274f - r;
  if (y < 0) r = -r;
  return r * 57.2957795f;
}

void Canvas::arc(float cx, float cy, float rOut, float thick, float a0, float a1,
                 const uint16_t* stops, int nStops, float gA0, float gA1,
                 uint8_t alpha, float feather) {
  if (a1 < a0) return;
  float hw = thick * 0.5f;
  float rm = rOut - hw;
  float ext = rOut + feather;
  const float D2R = (float)M_PI / 180.0f;
  float c0x = cx + rm * cosf(a0 * D2R), c0y = cy + rm * sinf(a0 * D2R);
  float c1x = cx + rm * cosf(a1 * D2R), c1y = cy + rm * sinf(a1 * D2R);
  float gSpan = (gA1 - gA0) != 0 ? (gA1 - gA0) : 1;

  float rIn = rm - hw - feather;
  if (rIn < 0) rIn = 0;
  // Угловой размер круглого конца, дальше от концов их можно не считать
  float capDeg = (hw + feather + 1) / max(rIn, 4.0f) / D2R;
  float invF = 1.0f / feather;
  int y0 = (int)floorf(cy - ext), y1 = (int)ceilf(cy + ext);
  clipY(y0, y1);
  for (int yy = y0; yy < y1; yy++) {
    float dy = yy + 0.5f - cy;
    // Обходим только кольцо: две горизонтальные полоски в этой строке
    float so = ext * ext - dy * dy;
    if (so <= 0) continue;
    float xo = sqrtf(so) + 1;
    float si = rIn * rIn - dy * dy;
    float xi = si > 0 ? sqrtf(si) - 1 : 0;
    if (xi < 0) xi = 0;
    int spans[2][2] = {
      {(int)floorf(cx - xo), (int)ceilf(cx - xi)},
      {(int)floorf(cx + xi), (int)ceilf(cx + xo)},
    };
    if (spans[0][1] >= spans[1][0]) { spans[0][1] = spans[1][1]; spans[1][0] = spans[1][1]; }
    for (int sp = 0; sp < 2; sp++)
    for (int xx = max(0, spans[sp][0]); xx < min(w_, spans[sp][1]); xx++) {
      float dx = xx + 0.5f - cx;
      float d = sqrtf(dx * dx + dy * dy);
      float radial = hw - fabsf(d - rm);
      if (radial < -feather) continue;

      float ang = fastAtan2Deg(dy, dx);
      while (ang < a0) ang += 360.0f;
      while (ang >= a0 + 360.0f) ang -= 360.0f;

      float cov, gAng;
      if (ang <= a1) {
        cov = clamp01(radial * invF + 0.5f);
        gAng = ang;
      } else {
        // За пределами угла: круглые концы, считаем только рядом с ними
        bool near0 = a0 + 360.0f - ang <= capDeg;
        bool near1 = ang - a1 <= capDeg;
        if (!near0 && !near1) continue;
        float e0 = 1e9f, e1 = 1e9f;
        if (near0) {
          float d0x = xx + 0.5f - c0x, d0y = yy + 0.5f - c0y;
          e0 = sqrtf(d0x * d0x + d0y * d0y);
        }
        if (near1) {
          float d1x = xx + 0.5f - c1x, d1y = yy + 0.5f - c1y;
          e1 = sqrtf(d1x * d1x + d1y * d1y);
        }
        float e = e0 < e1 ? e0 : e1;
        gAng = e0 < e1 ? a0 : a1;
        cov = clamp01((hw - e) * invF + 0.5f);
      }
      if (cov <= 0) continue;
      uint16_t c = nStops > 1 ? gradientAt(stops, nStops, (gAng - gA0) / gSpan) : stops[0];
      blend(xx, yy, c, (uint8_t)(cov * alpha));
    }
  }
}

bool PolarCache::build(float cx, float cy, float rMax) {
  cx_ = cx; cy_ = cy;
  x0_ = (int)floorf(cx - rMax - 1);
  y0_ = (int)floorf(cy - rMax - 1);
  size_ = (int)ceilf(rMax * 2 + 2);
  size_t n = (size_t)size_ * size_;
  ang_ = (uint16_t*)heap_caps_malloc(n * 2, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  dist_ = (uint16_t*)heap_caps_malloc(n * 2, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (!ang_ || !dist_) {
    free(ang_); free(dist_);
    ang_ = dist_ = nullptr;
    return false;
  }
  for (int j = 0; j < size_; j++)
    for (int i = 0; i < size_; i++) {
      float dx = x0_ + i + 0.5f - cx, dy = y0_ + j + 0.5f - cy;
      float a = atan2f(dy, dx) * 57.2957795f;
      if (a < 0) a += 360.0f;
      float d = sqrtf(dx * dx + dy * dy);
      ang_[j * size_ + i] = (uint16_t)min(35999.0f, a * 100.0f);
      dist_[j * size_ + i] = (uint16_t)min(65535.0f, d * 16.0f);
    }
  return true;
}

void Canvas::arc(const PolarCache& pc, float rOut, float thick, float a0, float a1,
                 const uint16_t* stops, int nStops, float gA0, float gA1,
                 uint8_t alpha, float feather) {
  if (!pc.ready()) {
    arc(pc.cx_, pc.cy_, rOut, thick, a0, a1, stops, nStops, gA0, gA1, alpha, feather);
    return;
  }
  if (a1 < a0) return;
  float hw = thick * 0.5f;
  float rm = rOut - hw;
  const float D2R = (float)M_PI / 180.0f;
  float cx = pc.cx_, cy = pc.cy_;
  float c0x = cx + rm * cosf(a0 * D2R), c0y = cy + rm * sinf(a0 * D2R);
  float c1x = cx + rm * cosf(a1 * D2R), c1y = cy + rm * sinf(a1 * D2R);
  float gSpan = (gA1 - gA0) != 0 ? (gA1 - gA0) : 1;
  float rIn = max(0.0f, rm - hw - feather);
  float capDeg = (hw + feather + 1) / max(rIn, 4.0f) / D2R;
  float invF = 1.0f / feather, invSpan = 1.0f / gSpan;
  // Быстрый отсев по расстоянию в целых числах
  int dLo = (int)floorf(rIn * 16), dHi = (int)ceilf((rm + hw + feather) * 16);

  int y0 = max(pc.y0_, bandY_), y1 = min(pc.y0_ + pc.size_, bandY_ + rows_);
  int x0 = max(pc.x0_, 0), x1 = min(pc.x0_ + pc.size_, w_);
  for (int yy = y0; yy < y1; yy++) {
    const uint16_t* drow = pc.dist_ + (yy - pc.y0_) * pc.size_ - pc.x0_;
    const uint16_t* arow = pc.ang_ + (yy - pc.y0_) * pc.size_ - pc.x0_;
    uint16_t* p = buf_ + (yy - bandY_) * w_;
    for (int xx = x0; xx < x1; xx++) {
      int di = drow[xx];
      if (di < dLo || di > dHi) continue;
      float d = di * (1.0f / 16);
      float radial = hw - fabsf(d - rm);
      if (radial < -feather) continue;
      float ang = arow[xx] * 0.01f;
      while (ang < a0) ang += 360.0f;
      while (ang >= a0 + 360.0f) ang -= 360.0f;

      float cov, gAng;
      if (ang <= a1) {
        cov = clamp01(radial * invF + 0.5f);
        gAng = ang;
      } else {
        bool near0 = a0 + 360.0f - ang <= capDeg;
        bool near1 = ang - a1 <= capDeg;
        if (!near0 && !near1) continue;
        float e0 = 1e9f, e1 = 1e9f;
        if (near0) { float ex = xx + 0.5f - c0x, ey = yy + 0.5f - c0y; e0 = sqrtf(ex * ex + ey * ey); }
        if (near1) { float ex = xx + 0.5f - c1x, ey = yy + 0.5f - c1y; e1 = sqrtf(ex * ex + ey * ey); }
        float e = e0 < e1 ? e0 : e1;
        gAng = e0 < e1 ? a0 : a1;
        cov = clamp01((hw - e) * invF + 0.5f);
      }
      if (cov <= 0) continue;
      uint16_t c = nStops > 1 ? gradientAt(stops, nStops, (gAng - gA0) * invSpan) : stops[0];
      uint8_t a = (uint8_t)(cov * alpha);
      p[xx] = (a == 255) ? c : mix(p[xx], c, a);
    }
  }
}

// =====================================================
// ЛИНИИ И ФИГУРЫ
// =====================================================
static inline float segDist(float px, float py, float ax, float ay, float bx, float by) {
  float vx = bx - ax, vy = by - ay;
  float wx = px - ax, wy = py - ay;
  float l2 = vx * vx + vy * vy;
  float t = l2 > 0 ? (wx * vx + wy * vy) / l2 : 0;
  t = clamp01(t);
  float dx = wx - t * vx, dy = wy - t * vy;
  return sqrtf(dx * dx + dy * dy);
}

void Canvas::line(float x0, float y0, float x1, float y1, float w, uint16_t c, uint8_t a) {
  float ext = w * 0.5f + 1;
  int bx0 = max(0, (int)floorf(min(x0, x1) - ext)), bx1 = min(w_, (int)ceilf(max(x0, x1) + ext));
  int by0 = (int)floorf(min(y0, y1) - ext), by1 = (int)ceilf(max(y0, y1) + ext);
  clipY(by0, by1);
  for (int yy = by0; yy < by1; yy++)
    for (int xx = bx0; xx < bx1; xx++) {
      float d = segDist(xx + 0.5f, yy + 0.5f, x0, y0, x1, y1);
      float cov = clamp01(w * 0.5f - d + 0.5f);
      if (cov > 0) blend(xx, yy, c, (uint8_t)(cov * a));
    }
}

void Canvas::fillPolygon(const float* xs, const float* ys, int n, uint16_t c, uint8_t a) {
  float minX = xs[0], maxX = xs[0], minY = ys[0], maxY = ys[0], area = 0;
  for (int i = 0; i < n; i++) {
    int j = (i + 1) % n;
    minX = min(minX, xs[i]); maxX = max(maxX, xs[i]);
    minY = min(minY, ys[i]); maxY = max(maxY, ys[i]);
    area += xs[i] * ys[j] - xs[j] * ys[i];
  }
  float sign = area > 0 ? 1.0f : -1.0f;
  int bx0 = max(0, (int)floorf(minX) - 1), bx1 = min(w_, (int)ceilf(maxX) + 1);
  int by0 = (int)floorf(minY) - 1, by1 = (int)ceilf(maxY) + 1;
  clipY(by0, by1);
  for (int yy = by0; yy < by1; yy++)
    for (int xx = bx0; xx < bx1; xx++) {
      float px = xx + 0.5f, py = yy + 0.5f, md = 1e9f;
      for (int i = 0; i < n; i++) {
        int j = (i + 1) % n;
        float ex = xs[j] - xs[i], ey = ys[j] - ys[i];
        float len = sqrtf(ex * ex + ey * ey);
        if (len <= 0) continue;
        float d = sign * (ex * (py - ys[i]) - ey * (px - xs[i])) / len;
        if (d < md) md = d;
      }
      float cov = clamp01(md + 0.5f);
      if (cov > 0) blend(xx, yy, c, (uint8_t)(cov * a));
    }
}

void Canvas::sparkline(const float* xs, const float* ys, int n, float w, uint16_t c,
                       float fillBottom, uint8_t fillAlpha) {
  if (n < 2) return;
  // Заливка под линией: прозрачность падает к низу
  if (fillAlpha) {
    int k = 0;
    for (int x = (int)ceilf(xs[0]); x <= (int)floorf(xs[n - 1]); x++) {
      float fx = x + 0.5f;
      while (k < n - 2 && fx > xs[k + 1]) k++;
      float span = xs[k + 1] - xs[k];
      float t = span > 0 ? clamp01((fx - xs[k]) / span) : 0;
      float yl = ys[k] + (ys[k + 1] - ys[k]) * t;
      float hgt = fillBottom - yl;
      if (hgt <= 0) continue;
      for (int y = (int)floorf(yl); y < (int)ceilf(fillBottom); y++) {
        float top = clamp01(y + 1 - yl);
        float fade = 1.0f - (y + 0.5f - yl) / hgt;
        blend(x, y, c, (uint8_t)(fillAlpha * clamp01(fade) * top));
      }
    }
  }
  // Сама линия: каждая точка считается один раз по ближайшим отрезкам
  float hw = w * 0.5f;
  int k = 0;
  int xa = max(0, (int)floorf(xs[0] - hw - 1)), xb = min(w_, (int)ceilf(xs[n - 1] + hw + 1));
  for (int x = xa; x < xb; x++) {
    float fx = x + 0.5f;
    while (k < n - 2 && fx > xs[k + 1]) k++;
    int s0 = max(0, k - 1), s1 = min(n - 2, k + 1);
    float ymin = 1e9f, ymax = -1e9f;
    for (int s = s0; s <= s1 + 1; s++) { ymin = min(ymin, ys[s]); ymax = max(ymax, ys[s]); }
    int y0 = (int)floorf(ymin - hw - 1), y1 = (int)ceilf(ymax + hw + 1);
    clipY(y0, y1);
    for (int y = y0; y < y1; y++) {
      float md = 1e9f;
      for (int s = s0; s <= s1; s++) {
        float d = segDist(fx, y + 0.5f, xs[s], ys[s], xs[s + 1], ys[s + 1]);
        if (d < md) md = d;
      }
      float cov = clamp01(hw - md + 0.5f);
      if (cov > 0) blend(x, y, c, (uint8_t)(cov * 255));
    }
  }
}

// =====================================================
// ТЕКСТ
// =====================================================
static uint16_t nextCode(const char*& s) {
  uint8_t c = (uint8_t)*s++;
  if (c < 0x80) return c;
  if ((c & 0xE0) == 0xC0) {
    uint16_t r = (c & 0x1F) << 6;
    if (*s) r |= (*s++ & 0x3F);
    return r;
  }
  if ((c & 0xF0) == 0xE0) {
    uint16_t r = (c & 0x0F) << 12;
    if (*s) r |= (*s++ & 0x3F) << 6;
    if (*s) r |= (*s++ & 0x3F);
    return r;
  }
  return '?';
}

const Glyph* Canvas::findGlyph(const Font& f, uint16_t code) const {
  int lo = 0, hi = f.count - 1;
  while (lo <= hi) {
    int mid = (lo + hi) >> 1;
    uint16_t v = f.codes[mid];
    if (v == code) return &f.glyphs[mid];
    if (v < code) lo = mid + 1; else hi = mid - 1;
  }
  return nullptr;
}

int Canvas::textWidth(const Font& f, const char* s) {
  int w = 0;
  while (*s) {
    const Glyph* g = findGlyph(f, nextCode(s));
    if (g) w += g->adv;
  }
  return w;
}

int Canvas::text(const Font& f, int x, int y, const char* s, uint16_t c, Align al, uint8_t a) {
  if (al != LEFT) {
    int w = textWidth(f, s);
    x -= (al == CENTER) ? w / 2 : w;
  }
  while (*s) {
    const Glyph* g = findGlyph(f, nextCode(s));
    if (!g) continue;
    int top = y + g->yo;
    if (top + g->h > bandY_ && top < bandY_ + rows_) {
      int rowBytes = (g->w + 1) >> 1;
      const uint8_t* bm = f.bmp + g->offset;
      for (int r = 0; r < g->h; r++) {
        int py = top + r;
        if (!inBand(py)) continue;
        const uint8_t* row = bm + r * rowBytes;
        for (int col = 0; col < g->w; col++) {
          uint8_t nib = (col & 1) ? (row[col >> 1] & 0x0F) : (row[col >> 1] >> 4);
          if (nib) blend(x + g->xo + col, py, c, (uint8_t)((nib * 17 * a) / 255));
        }
      }
    }
    x += g->adv;
  }
  return x;
}

}  // namespace gfx
