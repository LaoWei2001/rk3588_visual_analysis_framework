#define _XOPEN_SOURCE 700

#include "common/anonymous_intro.h"
#include "common/intro_notice.h"

#include <curses.h>
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


/*
 * Guy Fawkes mask outline.
 *
 * y > 0  : forehead / eyes
 * y = 0  : nose
 * y < 0  : mouth / chin
 */
static double mask_half_width(double y)
{
    static const double profile[][2] = {
        {-1.22, 0.10}, /* pointed chin */
        {-1.10, 0.22},
        {-0.90, 0.40},
        {-0.62, 0.57},
        {-0.25, 0.70},
        { 0.15, 0.77},
        { 0.48, 0.79}, /* cheek / eye area */
        { 0.80, 0.73},
        { 1.05, 0.61},
        { 1.22, 0.43}
    };

    size_t count = sizeof(profile) / sizeof(profile[0]);

    if (y <= profile[0][0])
        return profile[0][1];

    for (size_t i = 1; i < count; ++i)
    {
        if (y <= profile[i][0])
        {
            double t =
                (y - profile[i - 1][0]) /
                (profile[i][0] - profile[i - 1][0]);

            return profile[i - 1][1] +
                   t * (profile[i][1] - profile[i - 1][1]);
        }
    }

    return profile[count - 1][1];
}


/*
 * Long, narrow and slightly tilted eyes.
 *
 * Guy Fawkes mask eyes are not round.
 */
static bool inside_eye(double x, double y)
{
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
    double shape =
        (u * u) / (0.245 * 0.245) +
        (v * v) / (0.087 * 0.087);

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
static bool on_eyebrow(double x, double y)
{
    double d = fabs(x);

    if (d < 0.10 || d > 0.61)
        return false;

    double t = (d - 0.10) / 0.51;

    /*
     * Strong Guy Fawkes arch.
     */
    double brow_y =
        0.625 +
        0.12 * t -
        0.045 * sin(t * M_PI);

    return fabs(y - brow_y) < 0.030;
}


/*
 * Long narrow nose shadows.
 */
static bool on_nose_shadow(double x, double y)
{
    if (y < -0.10 || y > 0.37)
        return false;

    double progress = (0.37 - y) / 0.47;

    double nose_x =
        0.040 +
        progress * 0.055;

    return fabs(fabs(x) - nose_x) < 0.020;
}


/*
 * Nose tip / nostril shadow.
 */
static bool on_nostril(double x, double y)
{
    if (y < -0.17 || y > -0.06)
        return false;

    double dx = fabs(x) - 0.10;
    double dy = y + 0.115;

    return (dx * dx) / (0.065 * 0.065) +
           (dy * dy) / (0.038 * 0.038) < 1.0;
}


/*
 * Guy Fawkes moustache.
 *
 * Starts below the nose and curls upward at both ends.
 */
static bool on_moustache(double x, double y)
{
    double d = fabs(x);

    if (d < 0.035 || d > 0.69)
        return false;

    /*
     * Main moustache arm.
     *
     * Low near the centre, progressively rising outward.
     */
    double t = d / 0.69;

    double moustache_y =
        -0.245 +
        0.22 * t +
        0.15 * t * t;

    double thickness =
        0.050 -
        0.020 * t;

    bool main_arm =
        fabs(y - moustache_y) < thickness;

    /*
     * Sharply curled tips.
     */
    bool curled_tip = false;

    if (d > 0.52)
    {
        double tip_t = (d - 0.52) / 0.17;

        double tip_y =
            -0.045 +
            0.095 * sin(tip_t * M_PI * 0.72);

        curled_tip =
            fabs(y - tip_y) <
            (0.038 - 0.015 * tip_t);
    }

    return main_arm || curled_tip;
}


/*
 * Characteristic Guy Fawkes smirk.
 *
 * Centre is lower, corners rise.
 */
static bool on_smile(double x, double y)
{
    double d = fabs(x);

    if (d > 0.43)
        return false;

    double t = d / 0.43;

    double mouth_y =
        -0.455 +
        0.105 * t * t;

    return fabs(y - mouth_y) < 0.025;
}


/*
 * Small shadows extending from the mouth corners.
 */
static bool on_mouth_corner(double x, double y)
{
    double d = fabs(x);

    if (d < 0.36 || d > 0.58)
        return false;

    double t = (d - 0.36) / 0.22;

    double line_y =
        -0.405 +
        0.11 * t;

    return fabs(y - line_y) < 0.020;
}


/*
 * Long pointed goatee.
 */
static bool inside_goatee(double x, double y)
{
    if (y > -0.55 || y < -1.15)
        return false;

    double progress =
        (-0.55 - y) / 0.60;

    double width =
        0.145 * (1.0 - progress) +
        0.012;

    /*
     * A small split near the top makes it look less like
     * a solid black triangle.
     */
    if (y > -0.72 &&
        fabs(x) < 0.020)
    {
        return false;
    }

    return fabs(x) < width;
}


/*
 * Guy Fawkes cheek / smile wrinkles.
 */
static bool on_cheek_line(double x, double y)
{
    double d = fabs(x);

    if (d < 0.31 || d > 0.65)
        return false;

    double t =
        (d - 0.31) / 0.34;

    double line1 =
        -0.24 -
        0.17 * t;

    double line2 =
        -0.15 -
        0.13 * t;

    bool first =
        fabs(y - line1) < 0.018;

    bool second =
        d > 0.42 &&
        fabs(y - line2) < 0.015;

    return first || second;
}


/*
 * Slight shadow under the lower lip.
 */
static bool on_lower_lip_shadow(double x, double y)
{
    if (fabs(x) > 0.22)
        return false;

    double t = x / 0.22;

    double line_y =
        -0.535 -
        0.020 * (1.0 - t * t);

    return fabs(y - line_y) < 0.017;
}


/*
 * Small central line between nose and moustache.
 */
static bool on_philtrum(double x, double y)
{
    return fabs(x) < 0.018 &&
           y < -0.11 &&
           y > -0.235;
}


/*
 * Add subtle face sculpting.
 *
 * These are not black lines.
 * They simply reduce the surface brightness.
 */
static double face_shadow(double x, double y)
{
    double shadow = 0.0;

    /*
     * Darker temples.
     */
    if (fabs(x) > 0.48 && y > 0.05)
        shadow += 0.08;

    /*
     * Stronger shadows beside nose.
     */
    if (fabs(x) > 0.11 &&
        fabs(x) < 0.27 &&
        y > -0.18 &&
        y < 0.40)
    {
        shadow += 0.07;
    }

    /*
     * Lower cheek shadows.
     */
    if (fabs(x) > 0.40 &&
        y < -0.10 &&
        y > -0.68)
    {
        shadow += 0.08;
    }

    /*
     * Jaw.
     */
    if (y < -0.65)
        shadow += 0.04;

    return shadow;
}


static void mask_rectangle_bounds(int *top,
                                  int *bottom,
                                  int *left,
                                  int *right)
{
    int art_rows = LINES - intro_notice_reserved_rows();
    double vertical_scale = art_rows / 2.78;
    double horizontal_scale = vertical_scale * 2.52;

    if (horizontal_scale > (COLS - 4) / 2.85)
    {
        horizontal_scale = (COLS - 4) / 2.85;
    }
    *top = (int)lround((1.37 - 1.22) * vertical_scale);
    *bottom = (int)lround((1.37 + 1.22) * vertical_scale);
    *left = COLS / 2 - (int)ceil(0.80 * horizontal_scale) - 1;
    *right = COLS / 2 + (int)ceil(0.80 * horizontal_scale) + 1;
    if (*top < 0)
    {
        *top = 0;
    }
    if (*bottom >= art_rows)
    {
        *bottom = art_rows - 1;
    }
    if (*left < 1)
    {
        *left = 1;
    }
    if (*right >= COLS - 1)
    {
        *right = COLS - 2;
    }
}

static void draw_unrevealed_rectangle(bool use_colors,
                                      int first_row,
                                      int bottom,
                                      int left,
                                      int right,
                                      unsigned int generation)
{
    static const char random_characters[] =
        "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ"
        "abcdefghijklmnopqrstuvwxyz@#$%&*+=?";
    int attributes = A_DIM;

    if (use_colors)
    {
        attributes |= COLOR_PAIR(ANONYMOUS_COLOR_MASK);
    }
    attron(attributes);
    for (int row = first_row; row <= bottom; ++row)
    {
        for (int column = left; column <= right; ++column)
        {
            uint32_t value = (uint32_t)(row + 1) * UINT32_C(0x9e3779b1) ^
                             (uint32_t)(column + 1) * UINT32_C(0x85ebca6b) ^
                             (generation + 1U) * UINT32_C(0xc2b2ae35);

            value ^= value >> 16;
            value *= UINT32_C(0x7feb352d);
            value ^= value >> 15;
            value *= UINT32_C(0x846ca68b);
            value ^= value >> 16;
            mvaddch(row, column,
                    random_characters[value %
                                      (sizeof(random_characters) - 1)]);
        }
    }
    attroff(attributes);
}

static void draw_stationary_mask(bool use_colors,
                                 int last_visible_row,
                                 double light_scan_y)
{
    /*
     * Dark -> bright.
     */
    static const char face_ramp[] =
        ".,:-=+*#%@";

    int art_rows =
        LINES -
        intro_notice_reserved_rows();

    /*
     * Face height.
     */
    double vertical_scale =
        art_rows / 2.78;

    /*
     * Terminal characters are taller than they are wide.
     */
    double horizontal_scale =
        vertical_scale * 2.52;

    if (horizontal_scale >
        (COLS - 4) / 2.85)
    {
        horizontal_scale =
            (COLS - 4) / 2.85;
    }

    for (int row = 0;
         row < art_rows && row <= last_visible_row;
         ++row)
    {
        double y =
            1.37 -
            row / vertical_scale;

        for (int column = 1;
             column < COLS - 1;
             ++column)
        {
            double x =
                (column - COLS / 2.0) /
                horizontal_scale;

            double width =
                mask_half_width(y);

            bool in_mask =
                y >= -1.22 &&
                y <= 1.22 &&
                fabs(x) <= width;

            int attributes =
                A_NORMAL;

            char pixel = ' ';


            /*
             * ===========================
             * Mask
             * ===========================
             */
            if (in_mask)
            {
                /*
                 * Simulated curved surface.
                 */
                double curve =
                    sqrt(
                        fmax(
                            0.0,
                            1.0 -
                            (x * x) /
                            (width * width)));

                /*
                 * Bright centre,
                 * darker edges.
                 */
                double light =
                    0.50 +
                    0.40 * curve -
                    0.055 * fabs(x) +
                    0.025 * y;

                /* 完整面具出现后，一条柔和光带只从底部向顶部经过一次。 */
                if (light_scan_y >= -1.60 && light_scan_y <= 1.60)
                {
                    double distance = fabs(y - light_scan_y);

                    if (distance < 0.24)
                    {
                        light += 0.34 * (1.0 - distance / 0.24);
                    }
                }

                light -=
                    face_shadow(x, y);


                bool eye =
                    inside_eye(x, y);

                bool eyebrow =
                    on_eyebrow(x, y);

                bool nose =
                    on_nose_shadow(x, y);

                bool nostril =
                    on_nostril(x, y);

                bool moustache =
                    on_moustache(x, y);

                bool mouth =
                    on_smile(x, y);

                bool mouth_corner =
                    on_mouth_corner(x, y);

                bool goatee =
                    inside_goatee(x, y);

                bool cheek =
                    on_cheek_line(x, y);

                bool lower_lip =
                    on_lower_lip_shadow(x, y);

                bool philtrum =
                    on_philtrum(x, y);


                /*
                 * Completely black facial features.
                 */
                bool black_feature =
                    eye ||
                    eyebrow ||
                    moustache ||
                    mouth ||
                    goatee;

                /*
                 * Softer shadows.
                 */
                bool gray_feature =
                    nose ||
                    nostril ||
                    mouth_corner ||
                    cheek ||
                    lower_lip ||
                    philtrum;


                if (black_feature)
                {
                    /*
                     * Terminal background is black,
                     * so blank space produces
                     * a deep cut-out feature.
                     */
                    pixel = ' ';

                    attributes =
                        A_NORMAL;
                }
                else if (gray_feature)
                {
                    if (nostril ||
                        philtrum)
                    {
                        pixel = ':';
                    }
                    else
                    {
                        pixel = '.';
                    }

                    attributes =
                        A_DIM;
                }
                else
                {
                    /*
                     * Extra brightness on forehead,
                     * nose bridge and chin centre.
                     */
                    if (fabs(x) < 0.09 &&
                        y > -0.05 &&
                        y < 0.78)
                    {
                        light += 0.08;
                    }

                    if (fabs(x) < 0.17 &&
                        y < -0.65)
                    {
                        light += 0.04;
                    }

                    if (light < 0.05)
                        light = 0.05;

                    if (light > 1.0)
                        light = 1.0;

                    int tone =
                        (int)(
                            light *
                            (sizeof(face_ramp) - 2));

                    if (tone < 0)
                        tone = 0;

                    if (tone >
                        (int)sizeof(face_ramp) - 2)
                    {
                        tone =
                            (int)sizeof(face_ramp) - 2;
                    }

                    pixel =
                        face_ramp[tone];

                    attributes =
                        tone >= 7
                            ? A_BOLD
                            : A_NORMAL;
                }

                if (use_colors)
                {
                    attributes |=
                        COLOR_PAIR(
                            ANONYMOUS_COLOR_MASK);
                }
            }


            /*
             * Because erase() has already cleared
             * the terminal, black facial features
             * do not need to be explicitly drawn.
             */
            if (pixel != ' ')
            {
                attron(attributes);

                mvaddch(
                    row,
                    column,
                    pixel);

                attroff(attributes);
            }
        }
    }
}


void anonymous_intro_play(void)
{
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

    if (!stdscr ||
        isendwin() ||
        COLS < 48 ||
        LINES < 18)
    {
        return;
    }

    use_colors =
        has_colors() &&
        COLOR_PAIRS >
            ANONYMOUS_COLOR_MASK;

    if (use_colors)
    {
        init_pair(
            ANONYMOUS_COLOR_MASK,
            COLOR_WHITE,
            COLOR_BLACK);
    }

    original_columns =
        COLS;

    original_rows =
        LINES;

    art_rows =
        LINES -
        intro_notice_reserved_rows();
    (void)clock_gettime(CLOCK_MONOTONIC, &random_time);
    random_seed = (unsigned int)random_time.tv_nsec ^
                  (unsigned int)random_time.tv_sec ^
                  (unsigned int)getpid();
    flushinp();
    nodelay(
        stdscr,
        true);

    mask_rectangle_bounds(&rectangle_top,
                          &rectangle_bottom,
                          &rectangle_left,
                          &rectangle_right);
    reveal_rows = rectangle_bottom - rectangle_top + 1;

    /* 第一阶段：显示无边框的淡色矩形显影区域。 */
    erase();
    draw_unrevealed_rectangle(use_colors,
                              rectangle_top,
                              rectangle_bottom,
                              rectangle_left,
                              rectangle_right,
                              random_seed);
    intro_notice_draw(art_rows + 1);
    refresh();
    for (int elapsed = 0;
         elapsed < ANONYMOUS_RECTANGLE_HOLD_MS;
         elapsed += ANONYMOUS_TICK_MS)
    {
        int key = getch();

        if (COLS != original_columns ||
            LINES != original_rows ||
            (key != ERR && key != KEY_RESIZE))
        {
            stopped = true;
            break;
        }
        napms(ANONYMOUS_TICK_MS);
    }

    /* 第二阶段：不画边框，淡色矩形从上到下逐行替换为面具。 */
    for (int reveal_row = rectangle_top;
         !stopped && reveal_row <= rectangle_bottom;
         ++reveal_row)
    {
        int key;

        if (COLS != original_columns || LINES != original_rows)
        {
            stopped = true;
            break;
        }
        erase();
        draw_unrevealed_rectangle(use_colors,
                                  reveal_row + 1,
                                  rectangle_bottom,
                                  rectangle_left,
                                  rectangle_right,
                                  random_seed +
                                      (unsigned int)(reveal_row -
                                                     rectangle_top + 1));
        draw_stationary_mask(use_colors, reveal_row, -10.0);
        intro_notice_draw(art_rows + 1);
        refresh();
        key = getch();
        if (key != ERR && key != KEY_RESIZE)
        {
            stopped = true;
            break;
        }
        napms(ANONYMOUS_REVEAL_ROW_MS);
    }

    /* 第三阶段：完整面具上出现一次自下而上的光带。 */
    final_ticks = (ANONYMOUS_DISPLAY_MS -
                   ANONYMOUS_RECTANGLE_HOLD_MS -
                   reveal_rows * ANONYMOUS_REVEAL_ROW_MS) /
                  ANONYMOUS_TICK_MS;
    if (final_ticks < 12)
    {
        final_ticks = 12;
    }
    for (int tick = 0; !stopped && tick < final_ticks; ++tick)
    {
        int key;
        double progress = final_ticks > 1
                              ? (double)tick / (final_ticks - 1)
                              : 1.0;
        double light_scan_y = -1.42 + progress * 2.84;

        if (COLS != original_columns || LINES != original_rows)
        {
            break;
        }
        erase();
        draw_stationary_mask(use_colors, art_rows - 1, light_scan_y);
        intro_notice_draw(art_rows + 1);
        refresh();
        key = getch();
        if (key != ERR && key != KEY_RESIZE)
        {
            break;
        }
        napms(ANONYMOUS_TICK_MS);
    }

    nodelay(
        stdscr,
        false);
}
