#define _XOPEN_SOURCE 700

#include "common/cube_intro.h"
#include "common/intro_notice.h"

#include <curses.h>
#include <float.h>
#include <math.h>
#include <stdbool.h>
#include <stdlib.h>

#define CUBE_PI 3.14159265358979323846
#define CUBE_FRAMES 72
#define CUBE_FRAME_DELAY_MS 40
#define CUBE_FACE_STEPS 52
#define CUBE_COLOR 8
#define HELLO_GLYPH_WIDTH 5
#define HELLO_GLYPH_HEIGHT 7
#define HELLO_GLYPH_ADVANCE 6
#define HELLO_TEXT_WIDTH 29

typedef struct
{
    int columns;
    int rows;
    double scale;
    double angle;
    double pitch;
    double roll;
    double *depth;
    char *pixels;
    unsigned char *tones;
} CubeFrame;

static void rotate_y(double angle,
                     double x, double y, double z,
                     double *out_x, double *out_y, double *out_z)
{
    double cosine = cos(angle);
    double sine = sin(angle);

    *out_x = x * cosine + z * sine;
    *out_y = y;
    *out_z = -x * sine + z * cosine;
}

static void rotate_x(double angle,
                     double x, double y, double z,
                     double *out_x, double *out_y, double *out_z)
{
    double cosine = cos(angle);
    double sine = sin(angle);

    *out_x = x;
    *out_y = y * cosine - z * sine;
    *out_z = y * sine + z * cosine;
}

static void rotate_z(double angle,
                     double x, double y, double z,
                     double *out_x, double *out_y, double *out_z)
{
    double cosine = cos(angle);
    double sine = sin(angle);

    *out_x = x * cosine - y * sine;
    *out_y = x * sine + y * cosine;
    *out_z = z;
}

static void rotate_cube_vector(const CubeFrame *frame,
                               double x, double y, double z,
                               double *out_x, double *out_y, double *out_z)
{
    double first_x;
    double first_y;
    double first_z;
    double second_x;
    double second_y;
    double second_z;

    rotate_y(frame->angle, x, y, z,
             &first_x, &first_y, &first_z);
    rotate_x(frame->pitch, first_x, first_y, first_z,
             &second_x, &second_y, &second_z);
    rotate_z(frame->roll, second_x, second_y, second_z,
             out_x, out_y, out_z);
}

static void cube_plot(CubeFrame *frame,
                      double x, double y, double z,
                      double nx, double ny, double nz,
                      bool edge,
                      char greeting)
{
    static const char light_ramp[] = ".,:;-=+*#%@";
    const double camera_distance = 4.6;
    const double light_x = -0.38;
    const double light_y = 0.58;
    const double light_z = 0.72;
    double rotated_x;
    double rotated_y;
    double rotated_z;
    double rotated_nx;
    double rotated_ny;
    double rotated_nz;
    double perspective;
    double diffuse;
    double brightness;
    int screen_x;
    int screen_y;
    int index;
    int tone;

    rotate_cube_vector(frame, x, y, z,
                       &rotated_x, &rotated_y, &rotated_z);
    rotate_cube_vector(frame, nx, ny, nz,
                       &rotated_nx, &rotated_ny, &rotated_nz);
    if (camera_distance - rotated_z <= 0.2)
    {
        return;
    }

    perspective = camera_distance / (camera_distance - rotated_z);
    screen_x = frame->columns / 2 +
               (int)lround(rotated_x * frame->scale * perspective);
    screen_y = frame->rows / 2 -
               (int)lround(rotated_y * frame->scale * 0.50 * perspective);
    if (screen_x < 1 || screen_x >= frame->columns - 1 ||
        screen_y < 0 || screen_y >= frame->rows)
    {
        return;
    }

    index = screen_y * frame->columns + screen_x;
    if (greeting != '\0')
    {
        /* 文字是立方体表面的纹理，略微抬高以避免与面采样争夺深度。 */
        rotated_z += 0.008;
    }
    else if (edge)
    {
        /* 让棱线略微靠近镜头，避免被同一平面的采样点覆盖。 */
        rotated_z += 0.004;
    }
    if (rotated_z <= frame->depth[index])
    {
        return;
    }

    diffuse = rotated_nx * light_x +
              rotated_ny * light_y +
              rotated_nz * light_z;
    if (diffuse < 0.0)
    {
        diffuse = 0.0;
    }
    brightness = 0.18 + diffuse * 0.82;
    tone = 1 + (int)lround(
        brightness * (double)(sizeof(light_ramp) - 3));
    if (tone < 1)
    {
        tone = 1;
    }
    if (tone > (int)sizeof(light_ramp) - 2)
    {
        tone = (int)sizeof(light_ramp) - 2;
    }

    frame->depth[index] = rotated_z;
    frame->pixels[index] = greeting != '\0'
                               ? greeting
                               : edge ? '@' : light_ramp[tone];
    frame->tones[index] = greeting != '\0' || edge
                              ? (unsigned char)(sizeof(light_ramp) - 2)
                              : (unsigned char)tone;
}

static char hello_face_character(double horizontal, double vertical)
{
    static const char *const glyphs[5][HELLO_GLYPH_HEIGHT] = {
        {"#...#", "#...#", "#...#", "#####", "#...#", "#...#", "#...#"},
        {"#####", "#....", "#....", "####.", "#....", "#....", "#####"},
        {"#....", "#....", "#....", "#....", "#....", "#....", "#####"},
        {"#....", "#....", "#....", "#....", "#....", "#....", "#####"},
        {".###.", "#...#", "#...#", "#...#", "#...#", "#...#", ".###."}};
    static const char greeting[] = "HELLO";
    const double left = -0.87;
    const double right = 0.87;
    const double top = 0.42;
    const double bottom = -0.42;
    int column;
    int row;
    int letter;
    int glyph_column;

    if (horizontal < left || horizontal >= right ||
        vertical <= bottom || vertical > top)
    {
        return '\0';
    }
    column = (int)((horizontal - left) * HELLO_TEXT_WIDTH /
                   (right - left));
    row = (int)((top - vertical) * HELLO_GLYPH_HEIGHT /
                (top - bottom));
    if (column < 0 || column >= HELLO_TEXT_WIDTH ||
        row < 0 || row >= HELLO_GLYPH_HEIGHT)
    {
        return '\0';
    }
    letter = column / HELLO_GLYPH_ADVANCE;
    glyph_column = column % HELLO_GLYPH_ADVANCE;
    if (letter >= 5 || glyph_column >= HELLO_GLYPH_WIDTH ||
        glyphs[letter][row][glyph_column] != '#')
    {
        return '\0';
    }
    return greeting[letter];
}

static void cube_face(CubeFrame *frame, int axis, int side)
{
    for (int row = 0; row <= CUBE_FACE_STEPS; ++row)
    {
        double v = -1.0 + 2.0 * row / CUBE_FACE_STEPS;

        for (int column = 0; column <= CUBE_FACE_STEPS; ++column)
        {
            double u = -1.0 + 2.0 * column / CUBE_FACE_STEPS;
            double point[3] = {u, v, 0.0};
            double normal[3] = {0.0, 0.0, 0.0};
            bool edge = row == 0 || row == CUBE_FACE_STEPS ||
                        column == 0 || column == CUBE_FACE_STEPS;
            char greeting = '\0';

            if (axis == 0)
            {
                point[0] = (double)side;
                point[1] = v;
                point[2] = -side * u;
                greeting = hello_face_character(u, v);
            }
            else if (axis == 1)
            {
                point[0] = u;
                point[1] = (double)side;
                point[2] = v;
                if (side > 0)
                {
                    /* 上表面的文字朝向观察者，随顶面一起透视和旋转。 */
                    greeting = hello_face_character(u, -v);
                }
            }
            else
            {
                point[0] = side * u;
                point[1] = v;
                point[2] = (double)side;
                greeting = hello_face_character(u, v);
            }
            normal[axis] = (double)side;
            cube_plot(frame,
                      point[0], point[1], point[2],
                      normal[0], normal[1], normal[2],
                      edge, greeting);
        }
    }
}

static void build_cube(CubeFrame *frame)
{
    for (int axis = 0; axis < 3; ++axis)
    {
        cube_face(frame, axis, -1);
        cube_face(frame, axis, 1);
    }
}

void cube_intro_play(void)
{
    CubeFrame frame;
    double *depth;
    char *pixels;
    unsigned char *tones;
    int cells;
    int original_rows;
    bool use_colors;

    if (!stdscr || isendwin() || COLS < 48 || LINES < 18)
    {
        return;
    }

    use_colors = has_colors() && COLOR_PAIRS > CUBE_COLOR;
    if (use_colors)
    {
        init_pair(CUBE_COLOR, COLOR_WHITE, COLOR_BLACK);
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

    frame.columns = COLS;
    frame.rows = LINES - intro_notice_reserved_rows();
    frame.scale = (COLS - 4) / 3.45;
    if (frame.scale > (frame.rows - 2) / 1.52)
    {
        frame.scale = (frame.rows - 2) / 1.52;
    }
    frame.depth = depth;
    frame.pixels = pixels;
    frame.tones = tones;
    original_rows = LINES;

    flushinp();
    nodelay(stdscr, true);
    for (int animation_frame = 0;
         animation_frame < CUBE_FRAMES;
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

        frame.angle = 2.0 * CUBE_PI * animation_frame /
                      (CUBE_FRAMES - 1);
        frame.pitch = 0.52 + 0.18 * sin(frame.angle * 1.7);
        frame.roll = 0.10 * sin(frame.angle * 0.8);
        build_cube(&frame);

        erase();
        for (int row = 0; row < frame.rows; ++row)
        {
            for (int column = 0; column < frame.columns; ++column)
            {
                int index = row * frame.columns + column;
                int attributes;

                if (pixels[index] == ' ')
                {
                    continue;
                }
                attributes = tones[index] >= 8 ? A_BOLD : A_NORMAL;
                if (use_colors)
                {
                    attributes |= COLOR_PAIR(CUBE_COLOR);
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
        napms(CUBE_FRAME_DELAY_MS);
    }
    nodelay(stdscr, false);
    free(depth);
    free(pixels);
    free(tones);
}
