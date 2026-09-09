#define _XOPEN_SOURCE 700

#include "common/anonymous_intro.h"
#include "common/intro_notice.h"

#include "common/ansi_screen.h"
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#define ANONYMOUS_DISPLAY_MS 2880
#define ANONYMOUS_TICK_MS 40
#define ANONYMOUS_RECTANGLE_HOLD_MS 280
#define ANONYMOUS_REVEAL_ROW_MS 46

#define ANONYMOUS_COLOR_MASK 8
#define ANONYMOUS_HORIZONTAL_ASPECT 2.64
#define ANONYMOUS_MAX_HALF_WIDTH 0.84

/* 特征线的最小覆盖宽度会随终端单元格大小更新。 */
static double detail_radius_x = 0.020;
static double detail_radius_y = 0.030;

static double clamp_unit(double value) {
  if (value < 0.0)
    return 0.0;
  if (value > 1.0)
    return 1.0;
  return value;
}

static double bell(double value, double radius) {
  double normalized = value / radius;
  return exp(-(normalized * normalized));
}

static double smoothstep(double first, double last, double value) {
  double amount;

  if (last <= first)
    return value >= last ? 1.0 : 0.0;
  amount = clamp_unit((value - first) / (last - first));
  return amount * amount * (3.0 - 2.0 * amount);
}

/*
 * Guy Fawkes mask outline.
 *
 * y > 0  : forehead / eyes
 * y = 0  : nose
 * y < 0  : mouth / chin
 */
static double mask_half_width(double y) {
  static const double profile[][2] = {
      {-1.22, 0.10}, /* pointed chin */
      {-1.10, 0.23}, {-0.90, 0.42}, {-0.62, 0.60},
      {-0.25, 0.74}, {0.15, 0.82},  {0.48, 0.84}, /* cheek / eye area */
      {0.80, 0.77},  {1.05, 0.64},  {1.22, 0.45}};

  size_t count = sizeof(profile) / sizeof(profile[0]);

  if (y <= profile[0][0])
    return profile[0][1];

  for (size_t i = 1; i < count; ++i) {
    if (y <= profile[i][0]) {
      double t = (y - profile[i - 1][0]) / (profile[i][0] - profile[i - 1][0]);

      return profile[i - 1][1] + t * (profile[i][1] - profile[i - 1][1]);
    }
  }

  return profile[count - 1][1];
}

/*
 * Long, narrow and slightly tilted eyes.
 *
 * Guy Fawkes mask eyes are not round.
 */
static bool inside_eye(double x, double y) {
  double side = x < 0.0 ? -1.0 : 1.0;

  double cx = side * 0.31;
  double cy = 0.43;

  double dx = x - cx;
  double dy = y - cy;

  /*
   * Rotate each eye slightly upward toward the outside.
   */
  double angle = -side * 0.16;

  double ca = cos(angle);
  double sa = sin(angle);

  double u = dx * ca - dy * sa;
  double v = dx * sa + dy * ca;

  /*
   * Elliptical base.
   */
  double shape = (u * u) / (0.245 * 0.245) + (v * v) / (0.087 * 0.087);

  if (shape >= 1.0)
    return false;

  /*
   * Slightly cut the lower part to make the eyes look sharper.
   */
  if (v < -0.065)
    return false;

  return true;
}

/*
 * Thin arched eyebrows.
 *
 * Inner end is lower.
 * Outer end rises.
 */
static bool on_eyebrow(double x, double y) {
  double d = fabs(x);

  if (d < 0.10 || d > 0.61)
    return false;

  double t = (d - 0.10) / 0.51;

  /*
   * Strong Guy Fawkes arch.
   */
  double brow_y = 0.625 + 0.12 * t - 0.045 * sin(t * M_PI);

  return fabs(y - brow_y) < fmax(0.030, detail_radius_y * 0.78);
}

/*
 * Long narrow nose shadows.
 */
static bool on_nose_shadow(double x, double y) {
  if (y < -0.10 || y > 0.37)
    return false;

  double progress = (0.37 - y) / 0.47;

  double nose_x = 0.040 + progress * 0.055;

  return fabs(fabs(x) - nose_x) < fmax(0.020, detail_radius_x * 0.72);
}

/*
 * Nose tip / nostril shadow.
 */
static bool on_nostril(double x, double y) {
  if (y < -0.17 || y > -0.06)
    return false;

  double dx = fabs(x) - 0.10;
  double dy = y + 0.115;

  return (dx * dx) / (0.065 * 0.065) + (dy * dy) / (0.038 * 0.038) < 1.0;
}

/*
 * Guy Fawkes moustache.
 *
 * Starts below the nose and curls upward at both ends.
 */
static bool on_moustache(double x, double y) {
  double d = fabs(x);

  if (d < 0.035 || d > 0.69)
    return false;

  /*
   * Main moustache arm.
   *
   * Low near the centre, progressively rising outward.
   */
  double t = d / 0.69;

  /* 发端向上卷，但不会翻到鼻翼上方。 */
  double moustache_y = -0.245 + 0.13 * t + 0.07 * t * t;

  double thickness = 0.050 - 0.020 * t + detail_radius_y * 0.24;

  bool main_arm = fabs(y - moustache_y) < thickness;

  /*
   * Sharply curled tips.
   */
  bool curled_tip = false;

  if (d > 0.52) {
    double tip_t = (d - 0.52) / 0.17;

    double tip_y = -0.080 + 0.095 * sin(tip_t * M_PI * 0.72);

    curled_tip = fabs(y - tip_y) <
                 (0.038 - 0.015 * tip_t + detail_radius_y * 0.20);
  }

  return main_arm || curled_tip;
}

/*
 * Characteristic Guy Fawkes smirk.
 *
 * Centre is lower, corners rise.
 */
static bool on_smile(double x, double y) {
  double d = fabs(x);

  if (d > 0.43)
    return false;

  double t = d / 0.43;

  double mouth_y = -0.455 + 0.105 * t * t;

  return fabs(y - mouth_y) < fmax(0.025, detail_radius_y * 0.64);
}

/*
 * Small shadows extending from the mouth corners.
 */
static bool on_mouth_corner(double x, double y) {
  double d = fabs(x);

  if (d < 0.36 || d > 0.58)
    return false;

  double t = (d - 0.36) / 0.22;

  double line_y = -0.405 + 0.11 * t;

  return fabs(y - line_y) < fmax(0.020, detail_radius_y * 0.50);
}

/*
 * Long pointed goatee.
 */
static bool inside_goatee(double x, double y) {
  if (y > -0.55 || y < -1.15)
    return false;

  double progress = (-0.55 - y) / 0.60;

  double width = 0.145 * (1.0 - progress) + 0.012;

  /*
   * A small split near the top makes it look less like
   * a solid black triangle.
   */
  if (y > -0.72 && fabs(x) < 0.020) {
    return false;
  }

  return fabs(x) < width;
}

/*
 * Guy Fawkes cheek / smile wrinkles.
 */
static bool on_cheek_line(double x, double y) {
  double d = fabs(x);

  if (d < 0.31 || d > 0.65)
    return false;

  double t = (d - 0.31) / 0.34;

  double line1 = -0.24 - 0.17 * t;

  double line2 = -0.15 - 0.13 * t;

  bool first = fabs(y - line1) < fmax(0.018, detail_radius_y * 0.42);

  bool second =
      d > 0.42 && fabs(y - line2) < fmax(0.015, detail_radius_y * 0.36);

  return first || second;
}

/*
 * Slight shadow under the lower lip.
 */
static bool on_lower_lip_shadow(double x, double y) {
  if (fabs(x) > 0.22)
    return false;

  double t = x / 0.22;

  double line_y = -0.535 - 0.020 * (1.0 - t * t);

  return fabs(y - line_y) < fmax(0.017, detail_radius_y * 0.42);
}

/*
 * Small central line between nose and moustache.
 */
static bool on_philtrum(double x, double y) {
  return fabs(x) < fmax(0.018, detail_radius_x * 0.55) && y < -0.11 &&
         y > -0.235;
}

/* 眼窝下缘和下巴折线使五官在矮终端中也不会糊成平面。 */
static bool on_eye_crease(double x, double y) {
  double distance = fabs(x);
  double amount;
  double crease_y;

  if (distance < 0.13 || distance > 0.61)
    return false;
  amount = (distance - 0.13) / 0.48;
  crease_y = 0.315 - 0.055 * amount - 0.025 * sin(amount * M_PI);
  return fabs(y - crease_y) < fmax(0.014, detail_radius_y * 0.32);
}

static bool on_chin_crease(double x, double y) {
  double amount;
  double crease_y;

  if (fabs(x) > 0.34)
    return false;
  amount = x / 0.34;
  crease_y = -0.735 + 0.040 * amount * amount;
  return fabs(y - crease_y) < fmax(0.014, detail_radius_y * 0.30);
}

/*
 * Add subtle face sculpting.
 *
 * These are not black lines.
 * They simply reduce the surface brightness.
 */
static double face_shadow(double x, double y) {
  double distance = fabs(x);
  double shadow = 0.0;

  /* 颞部、眼窝和鼻翼使用连续阴影，避免出现块状分层。 */
  shadow += 0.13 * smoothstep(0.46, 0.76, distance) *
            smoothstep(-0.10, 0.58, y);
  shadow += 0.18 * bell(distance - 0.31, 0.20) * bell(y - 0.42, 0.22);
  shadow += 0.12 * bell(distance - 0.17, 0.085) * bell(y - 0.08, 0.36);
  shadow += 0.10 * bell(distance - 0.13, 0.12) * bell(y + 0.13, 0.11);

  /* 颧骨下方、下唇与下颌的遮蔽。 */
  shadow += 0.10 * bell(distance - 0.43, 0.19) * bell(y + 0.28, 0.30);
  shadow += 0.09 * bell(x, 0.27) * bell(y + 0.58, 0.12);
  shadow += 0.08 * smoothstep(0.55, 1.12, -y) *
            smoothstep(0.20, 0.68, distance);
  return shadow;
}

static double mask_surface_light(double x, double y, double width,
                                 double light_scan_y) {
  double nx = width > 0.0 ? x / width : 0.0;
  double ny = y / 1.22;
  double nz = sqrt(fmax(0.0, 1.0 - nx * nx));
  double key =
      clamp_unit(-0.43 * nx + 0.17 * ny + 0.86 * nz);
  double fill =
      clamp_unit(0.40 * nx + 0.06 * ny + 0.82 * nz);
  double light = 0.06 + 0.44 * key + 0.13 * fill;
  double specular =
      clamp_unit(-0.34 * nx + 0.13 * ny + 0.93 * nz);

  /* 正面不是简单圆柱：额头、鼻梁、颧骨和下巴分别捕光。 */
  light += 0.08 * pow(specular, 12.0);
  light += 0.08 * bell(x + 0.18, 0.30) * bell(y - 0.78, 0.34);
  light += 0.15 * bell(x + 0.015, 0.105) * bell(y - 0.09, 0.50);
  light += 0.08 * bell(fabs(x) - 0.37, 0.15) * bell(y + 0.03, 0.25);
  light += 0.06 * bell(x, 0.19) * bell(y + 0.84, 0.24);
  light -= face_shadow(x, y);
  light -= 0.11 * pow(fabs(nx), 3.0);

  /* 斜向柔光带按曲面法线增亮，不再是一条平的水平白线。 */
  if (light_scan_y >= -1.60 && light_scan_y <= 1.60) {
    double distance = fabs((y - light_scan_y) + 0.18 * x);

    if (distance < 0.25) {
      double band = 1.0 - smoothstep(0.0, 0.25, distance);
      light += 0.38 * band * (0.30 + 0.70 * nz);
    }
  }
  return clamp_unit(light);
}

static void mask_rectangle_bounds(int *top, int *bottom, int *left,
                                  int *right) {
  int art_rows = LINES - intro_notice_reserved_rows();
  double vertical_scale = art_rows / 2.78;
  double horizontal_scale = vertical_scale * ANONYMOUS_HORIZONTAL_ASPECT;

  if (horizontal_scale > (COLS - 4) / 2.85) {
    horizontal_scale = (COLS - 4) / 2.85;
  }
  *top = (int)lround((1.37 - 1.22) * vertical_scale);
  *bottom = (int)lround((1.37 + 1.22) * vertical_scale);
  *left = COLS / 2 -
          (int)ceil(ANONYMOUS_MAX_HALF_WIDTH * horizontal_scale) - 1;
  *right = COLS / 2 +
           (int)ceil(ANONYMOUS_MAX_HALF_WIDTH * horizontal_scale) + 1;
  if (*top < 0) {
    *top = 0;
  }
  if (*bottom >= art_rows) {
    *bottom = art_rows - 1;
  }
  if (*left < 1) {
    *left = 1;
  }
  if (*right >= COLS - 1) {
    *right = COLS - 2;
  }
}

static void draw_unrevealed_rectangle(bool use_colors, int first_row,
                                      int bottom, int left, int right,
                                      unsigned int generation) {
  static const char random_characters[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ"
                                          "abcdefghijklmnopqrstuvwxyz@#$%&*+=?";
  int attributes = A_DIM;

  if (use_colors) {
    attributes |= COLOR_PAIR(ANONYMOUS_COLOR_MASK);
  }
  attron(attributes);
  for (int row = first_row; row <= bottom; ++row) {
    for (int column = left; column <= right; ++column) {
      uint32_t value = (uint32_t)(row + 1) * UINT32_C(0x9e3779b1) ^
                       (uint32_t)(column + 1) * UINT32_C(0x85ebca6b) ^
                       (generation + 1U) * UINT32_C(0xc2b2ae35);

      value ^= value >> 16;
      value *= UINT32_C(0x7feb352d);
      value ^= value >> 15;
      value *= UINT32_C(0x846ca68b);
      value ^= value >> 16;
      mvaddch(row, column,
              random_characters[value % (sizeof(random_characters) - 1)]);
    }
  }
  attroff(attributes);
}

static void draw_stationary_mask(bool use_colors, int last_visible_row,
                                 double light_scan_y) {
  /* 更细的字符密度梯度，与 dim/normal/bold 组合后提供更多灰阶。 */
  static const char face_ramp[] = ".,:;irsXA253hMHGS#9B&@";
  const int tone_count = (int)sizeof(face_ramp) - 1;

  int art_rows = LINES - intro_notice_reserved_rows();

  /*
   * Face height.
   */
  double vertical_scale = art_rows / 2.78;

  /*
   * Terminal characters are taller than they are wide.
   */
  double horizontal_scale = vertical_scale * ANONYMOUS_HORIZONTAL_ASPECT;

  if (horizontal_scale > (COLS - 4) / 2.85) {
    horizontal_scale = (COLS - 4) / 2.85;
  }
  detail_radius_x = fmax(0.018, 0.50 / horizontal_scale);
  detail_radius_y = fmax(0.022, 0.48 / vertical_scale);

  for (int row = 0; row < art_rows && row <= last_visible_row; ++row) {
    double y = 1.37 - row / vertical_scale;

    for (int column = 1; column < COLS - 1; ++column) {
      double x = (column - COLS / 2.0) / horizontal_scale;

      double width = mask_half_width(y);

      bool in_mask = y >= -1.22 && y <= 1.22 && fabs(x) <= width;

      int attributes = A_NORMAL;

      char pixel = ' ';

      /*
       * ===========================
      * Mask
      * ===========================
      */
      if (in_mask) {
        double light = mask_surface_light(x, y, width, light_scan_y);
        int tone;

        bool eye = inside_eye(x, y);

        bool eyebrow = on_eyebrow(x, y);

        bool nose = on_nose_shadow(x, y);

        bool nostril = on_nostril(x, y);

        bool moustache = on_moustache(x, y);

        bool mouth = on_smile(x, y);

        bool mouth_corner = on_mouth_corner(x, y);

        bool goatee = inside_goatee(x, y);

        bool cheek = on_cheek_line(x, y);

        bool lower_lip = on_lower_lip_shadow(x, y);

        bool philtrum = on_philtrum(x, y);

        bool eye_crease = on_eye_crease(x, y);

        bool chin_crease = on_chin_crease(x, y);

        /*
         * Completely black facial features.
         */
        bool black_feature =
            eye || eyebrow || nostril || moustache || mouth || goatee;

        /*
         * Softer shadows.
         */
        bool gray_feature = nose || mouth_corner || cheek || lower_lip ||
                            philtrum || eye_crease || chin_crease;

        tone = (int)lround(light * (tone_count - 1));
        if (tone < 0)
          tone = 0;
        if (tone >= tone_count)
          tone = tone_count - 1;

        if (black_feature) {
          /*
           * Terminal background is black,
           * so blank space produces
           * a deep cut-out feature.
           */
          pixel = ' ';

          attributes = A_NORMAL;
        } else if (gray_feature) {
          int depth = (philtrum || nose) ? 8 : 6;

          tone = tone > depth ? tone - depth : 0;
          pixel = face_ramp[tone];
          attributes = tone < tone_count / 3 ? A_DIM : A_NORMAL;
        } else {
          pixel = face_ramp[tone];
          if (tone < tone_count / 4) {
            attributes = A_DIM;
          } else if (tone >= tone_count * 3 / 4) {
            attributes = A_BOLD;
          }
        }

        if (use_colors) {
          attributes |= COLOR_PAIR(ANONYMOUS_COLOR_MASK);
        }
      }

      /*
       * Because erase() has already cleared
       * the terminal, black facial features
       * do not need to be explicitly drawn.
       */
      if (pixel != ' ') {
        attron(attributes);

        mvaddch(row, column, pixel);

        attroff(attributes);
      }
    }
  }
}

void anonymous_intro_play(void) {
  ansi_screen_sync();
  int original_columns;
  int original_rows;
  int art_rows;
  int rectangle_top;
  int rectangle_bottom;
  int rectangle_left;
  int rectangle_right;
  int reveal_rows;
  int final_ticks;
  struct timespec random_time = {0};
  unsigned int random_seed;
  bool use_colors;
  bool stopped = false;

  if (!stdscr || isendwin() || COLS < 48 || LINES < 18) {
    return;
  }

  use_colors = has_colors() && COLOR_PAIRS > ANONYMOUS_COLOR_MASK;

  if (use_colors) {
    init_pair(ANONYMOUS_COLOR_MASK, COLOR_WHITE, COLOR_BLACK);
  }

  original_columns = COLS;

  original_rows = LINES;

  art_rows = LINES - intro_notice_reserved_rows();
  (void)clock_gettime(CLOCK_MONOTONIC, &random_time);
  random_seed = (unsigned int)random_time.tv_nsec ^
                (unsigned int)random_time.tv_sec ^ (unsigned int)getpid();
  flushinp();
  nodelay(stdscr, true);

  mask_rectangle_bounds(&rectangle_top, &rectangle_bottom, &rectangle_left,
                        &rectangle_right);
  reveal_rows = rectangle_bottom - rectangle_top + 1;

  /* 第一阶段：显示无边框的淡色矩形显影区域。 */
  erase();
  draw_unrevealed_rectangle(use_colors, rectangle_top, rectangle_bottom,
                            rectangle_left, rectangle_right, random_seed);
  intro_notice_draw(art_rows + 1);
  refresh();
  for (int elapsed = 0; elapsed < ANONYMOUS_RECTANGLE_HOLD_MS;
       elapsed += ANONYMOUS_TICK_MS) {
    int key = getch();

    if (COLS != original_columns || LINES != original_rows ||
        (key != ERR && key != KEY_RESIZE)) {
      stopped = true;
      break;
    }
    napms(ANONYMOUS_TICK_MS);
  }

  /* 第二阶段：不画边框，淡色矩形从上到下逐行替换为面具。 */
  for (int reveal_row = rectangle_top;
       !stopped && reveal_row <= rectangle_bottom; ++reveal_row) {
    int key;

    if (COLS != original_columns || LINES != original_rows) {
      stopped = true;
      break;
    }
    erase();
    draw_unrevealed_rectangle(
        use_colors, reveal_row + 1, rectangle_bottom, rectangle_left,
        rectangle_right,
        random_seed + (unsigned int)(reveal_row - rectangle_top + 1));
    draw_stationary_mask(use_colors, reveal_row, -10.0);
    intro_notice_draw(art_rows + 1);
    refresh();
    key = getch();
    if (key != ERR && key != KEY_RESIZE) {
      stopped = true;
      break;
    }
    napms(ANONYMOUS_REVEAL_ROW_MS);
  }

  /* 第三阶段：完整面具上出现一次自下而上的光带。 */
  final_ticks = (ANONYMOUS_DISPLAY_MS - ANONYMOUS_RECTANGLE_HOLD_MS -
                 reveal_rows * ANONYMOUS_REVEAL_ROW_MS) /
                ANONYMOUS_TICK_MS;
  if (final_ticks < 12) {
    final_ticks = 12;
  }
  for (int tick = 0; !stopped && tick < final_ticks; ++tick) {
    int key;
    double progress = final_ticks > 1 ? (double)tick / (final_ticks - 1) : 1.0;
    double light_scan_y = -1.42 + progress * 2.84;

    if (COLS != original_columns || LINES != original_rows) {
      break;
    }
    erase();
    draw_stationary_mask(use_colors, art_rows - 1, light_scan_y);
    intro_notice_draw(art_rows + 1);
    refresh();
    key = getch();
    if (key != ERR && key != KEY_RESIZE) {
      break;
    }
    napms(ANONYMOUS_TICK_MS);
  }

  nodelay(stdscr, false);
}
