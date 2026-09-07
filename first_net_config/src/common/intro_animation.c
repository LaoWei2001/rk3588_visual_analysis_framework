#define _XOPEN_SOURCE 700

#include "common/intro_animation.h"
#include "common/intro_notice.h"

#include <curses.h>
#include <float.h>
#include <math.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#define INTRO_PI 3.14159265358979323846
#define INTRO_FRAMES 72
#define INTRO_FRAME_DELAY_MS 40
#define INTRO_COLOR_MASK 8

typedef struct
{
    int columns;
    int rows;
    double scale;
    double angle;
    double tilt;
    double *depth;
    char *pixels;
    unsigned char *tones;
} IntroFrame;

enum
{
    INTRO_MATERIAL_MASK = 1,
    INTRO_MATERIAL_VOID
};

static void intro_plot(IntroFrame *frame,
                       double x,
                       double y,
                       double z,
                       double nx,
                       double ny,
                       double nz,
                       unsigned char material)
{
    static const char mask_ramp[] = " .,:-=+*#%@";
    const char *ramp = mask_ramp;
    size_t ramp_length = strlen(ramp);
    double cosine = cos(frame->angle);
    double sine = sin(frame->angle);
    double tilt_cosine = cos(frame->tilt);
    double tilt_sine = sin(frame->tilt);
    double rotated_x = x * cosine + z * sine;
    double first_z = -x * sine + z * cosine;
    double rotated_y = y * tilt_cosine - first_z * tilt_sine;
    double rotated_z = y * tilt_sine + first_z * tilt_cosine;
    double rotated_nx = nx * cosine + nz * sine;
    double first_nz = -nx * sine + nz * cosine;
    double rotated_ny = ny * tilt_cosine - first_nz * tilt_sine;
    double rotated_nz = ny * tilt_sine + first_nz * tilt_cosine;
    double camera_distance = 4.2;
    double perspective;
    double light;
    double normal_length;
    int screen_x;
    int screen_y;
    int index;
    int tone;

    if (!frame || camera_distance - rotated_z <= 0.2)
    {
        return;
    }
    perspective = camera_distance / (camera_distance - rotated_z);
    screen_x = frame->columns / 2 +
               (int)lround(rotated_x * frame->scale * perspective);
    screen_y = frame->rows / 2 -
               (int)lround(rotated_y * frame->scale * 0.52 * perspective);
    if (screen_x < 1 || screen_x >= frame->columns - 1 ||
        screen_y < 0 || screen_y >= frame->rows)
    {
        return;
    }
    index = screen_y * frame->columns + screen_x;
    if (rotated_z <= frame->depth[index])
    {
        return;
    }
    if (material == INTRO_MATERIAL_VOID)
    {
        /* 深度空白让旋转时背面的面具表面不会穿过五官开口。 */
        frame->depth[index] = rotated_z;
        frame->pixels[index] = ' ';
        frame->tones[index] = 0;
        return;
    }
    normal_length = sqrt(rotated_nx * rotated_nx +
                         rotated_ny * rotated_ny +
                         rotated_nz * rotated_nz);
    if (normal_length < 0.0001)
    {
        normal_length = 1.0;
    }
    light = (-0.35 * rotated_nx +
             0.55 * rotated_ny +
             0.95 * rotated_nz) /
            normal_length;
    light = (light + 1.0) * 0.5;
    if (light < 0.08)
    {
        light = 0.08;
    }
    if (light > 1.0)
    {
        light = 1.0;
    }
    tone = 1 + (int)(light * (double)(ramp_length - 2));
    frame->depth[index] = rotated_z;
    frame->pixels[index] = ramp[tone];
    frame->tones[index] = (unsigned char)tone;
}

static double intro_mask_half_width(double y)
{
    static const double profile[][2] = {
        {-1.48, 0.12}, {-1.08, 0.26}, {-0.58, 0.42},
        {0.02, 0.59},  {0.53, 0.72},  {1.00, 0.68},
        {1.33, 0.45}};
    size_t count = sizeof(profile) / sizeof(profile[0]);

    if (y <= profile[0][0])
    {
        return profile[0][1];
    }
    for (size_t index = 1; index < count; ++index)
    {
        if (y <= profile[index][0])
        {
            double position = (y - profile[index - 1][0]) /
                              (profile[index][0] - profile[index - 1][0]);
            return profile[index - 1][1] +
                   position * (profile[index][1] - profile[index - 1][1]);
        }
    }
    return profile[count - 1][1];
}

static bool intro_inside_mask_eye(double x, double y)
{
    double side = x < 0.0 ? -1.0 : 1.0;
    double dx = x - side * 0.32;
    double dy = y - 0.65;
    double sine = side * 0.24;
    double cosine = sqrt(1.0 - sine * sine);
    double u = dx * cosine - dy * sine;
    double v = dx * sine + dy * cosine;
    double ellipse = (u * u) / (0.22 * 0.22) +
                     (v * v) / (0.29 * 0.29);

    /* The lower taper gives the iconic long, mournful eye openings. */
    return ellipse < 1.0 &&
           fabs(u) < 0.19 - 0.035 * (0.29 - v);
}

static bool intro_inside_mask_nose(double x, double y)
{
    if (y < 0.08 || y > 0.36)
    {
        return false;
    }
    return fabs(x) < 0.030 + (0.36 - y) * 0.18;
}

static bool intro_inside_mask_mouth(double x, double y)
{
    double vertical = (y + 0.63) / 0.39;
    double horizontal = x / (0.19 - 0.035 * fabs(vertical));

    return pow(fabs(horizontal), 1.7) +
               pow(fabs(vertical), 2.2) <
           1.0;
}

static void intro_build_ghostface_mask(IntroFrame *frame)
{
    /* A curved, elongated solid mask with actual holes, not a flat sprite. */
    for (int vertical = 0; vertical <= 82; ++vertical)
    {
        double y = -1.48 + vertical * (2.81 / 82.0);
        double half_width = intro_mask_half_width(y);

        for (int horizontal = -38; horizontal <= 38; ++horizontal)
        {
            double position = horizontal / 38.0;
            double x = position * half_width;
            double nose_relief = 0.16 * exp(-12.0 * x * x) *
                                 exp(-3.0 * (y - 0.12) * (y - 0.12));
            double cheek_relief = 0.045 * cos(y * 4.2) *
                                  (1.0 - position * position);
            double z = 0.70 + 0.28 * (1.0 - position * position) +
                       nose_relief + cheek_relief;

            if (intro_inside_mask_eye(x, y) ||
                intro_inside_mask_nose(x, y) ||
                intro_inside_mask_mouth(x, y))
            {
                intro_plot(frame, x, y, z + 0.012,
                           0.0, 0.0, 1.0,
                           INTRO_MATERIAL_VOID);
                continue;
            }
            intro_plot(frame, x, y, z,
                       0.78 * position,
                       -0.08 * sin(y * 4.2),
                       1.0,
                       INTRO_MATERIAL_MASK);
        }
    }

    /* Give both side edges thickness so the profile remains physical in motion. */
    for (int vertical = 0; vertical <= 82; ++vertical)
    {
        double y = -1.48 + vertical * (2.81 / 82.0);
        double half_width = intro_mask_half_width(y);

        for (int side = -1; side <= 1; side += 2)
        {
            for (int layer = 0; layer <= 8; ++layer)
            {
                double z = 0.58 + layer * 0.016;

                intro_plot(frame, side * half_width, y, z,
                           side, 0.0, 0.25,
                           INTRO_MATERIAL_MASK);
            }
        }
    }
}

void intro_animation_play(void)
{
    int cells;
    double *depth;
    char *pixels;
    unsigned char *tones;
    IntroFrame frame;
    int original_rows;
    bool use_colors;

    if (!stdscr || isendwin() || COLS < 48 || LINES < 18)
    {
        return;
    }
    use_colors = has_colors() && COLOR_PAIRS > INTRO_COLOR_MASK;
    if (use_colors)
    {
        init_pair(INTRO_COLOR_MASK, COLOR_WHITE, COLOR_BLACK);
    }

    cells = COLS * LINES;
    depth = malloc((size_t)cells * sizeof(*depth));
    pixels = malloc((size_t)cells * sizeof(*pixels));
    tones = malloc((size_t)cells * sizeof(*tones));
    if (!depth || !pixels || !tones)
    {
        free(depth);
        free(pixels);
        free(tones);
        return;
    }
    flushinp();
    nodelay(stdscr, true);
    original_rows = LINES;
    frame.columns = COLS;
    frame.rows = LINES - intro_notice_reserved_rows();
    frame.scale = (frame.rows - 1) / (3.55 * 0.52);
    if (frame.scale > COLS / 3.4)
    {
        frame.scale = COLS / 3.4;
    }
    frame.depth = depth;
    frame.pixels = pixels;
    frame.tones = tones;

    for (int animation_frame = 0;
         animation_frame < INTRO_FRAMES;
         ++animation_frame)
    {
        int key;

        if (COLS != frame.columns || LINES != original_rows)
        {
            break;
        }
        for (int index = 0; index < cells; ++index)
        {
            depth[index] = -DBL_MAX;
            pixels[index] = ' ';
            tones[index] = 0;
        }
        frame.angle = 2.0 * INTRO_PI * animation_frame /
                      (INTRO_FRAMES - 1);
        frame.tilt = 0.10 * sin(frame.angle * 2.0);
        intro_build_ghostface_mask(&frame);

        erase();
        for (int row = 0; row < frame.rows; ++row)
        {
            for (int column = 0; column < COLS; ++column)
            {
                int index = row * COLS + column;
                int attributes;

                if (pixels[index] == ' ')
                {
                    continue;
                }
                attributes = tones[index] > 7 ? A_BOLD : A_NORMAL;
                if (use_colors)
                {
                    attributes |= COLOR_PAIR(INTRO_COLOR_MASK);
                }
                attron(attributes);
                mvaddch(row, column, pixels[index]);
                attroff(attributes);
            }
        }
        intro_notice_draw(frame.rows + 1);
        refresh();
        key = getch();
        if (key != ERR && key != KEY_RESIZE)
        {
            break;
        }
        napms(INTRO_FRAME_DELAY_MS);
    }
    nodelay(stdscr, false);
    free(depth);
    free(pixels);
    free(tones);
}
