#define _POSIX_C_SOURCE 200809L

#include "common/wifi_signal_parser.h"
#include "common/nmcli_parser.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool active_marker(const char *text)
{
    return text &&
           (strcmp(text, "*") == 0 ||
            strcmp(text, "yes") == 0 ||
            strcmp(text, "true") == 0);
}

static bool parse_percentage(const char *text, int *percentage)
{
    char *end = NULL;
    long value;

    if (!text || !percentage || text[0] == '\0')
    {
        return false;
    }

    errno = 0;
    value = strtol(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' ||
        value < 0 || value > 100)
    {
        return false;
    }

    *percentage = (int)value;
    return true;
}

bool parse_active_wifi_access_point(char *output,
                                    WifiAccessPointSample *sample)
{
    char *saveptr = NULL;
    char *line;

    if (!output || !sample)
    {
        return false;
    }

    memset(sample, 0, sizeof(*sample));
    sample->percentage = -1;
    line = strtok_r(output, "\n", &saveptr);

    while (line)
    {
        char *fields[4] = {0};
        int field_count = split_nmcli_escaped_fields(line, fields, 4);

        if (field_count == 4 && active_marker(fields[0]))
        {
            int percentage;

            snprintf(sample->ssid, sizeof(sample->ssid), "%s", fields[1]);
            snprintf(sample->bssid, sizeof(sample->bssid), "%s", fields[2]);
            if (parse_percentage(fields[3], &percentage))
            {
                sample->percentage = percentage;
            }
            return true;
        }

        line = strtok_r(NULL, "\n", &saveptr);
    }

    return false;
}

bool parse_iw_signal_dbm(const char *output, int *dbm)
{
    const char *line;

    if (!output || !dbm)
    {
        return false;
    }

    line = output;
    while (*line)
    {
        const char *content = line;
        const char *next = strchr(line, '\n');

        while (*content == ' ' || *content == '\t')
        {
            ++content;
        }

        if (strncmp(content, "signal:", 7) == 0)
        {
            char *end = NULL;
            long value;

            content += 7;
            while (*content == ' ' || *content == '\t')
            {
                ++content;
            }

            errno = 0;
            value = strtol(content, &end, 10);
            if (errno == 0 && end != content &&
                value >= -127 && value <= 0)
            {
                *dbm = (int)value;
                return true;
            }
        }

        if (!next)
        {
            break;
        }
        line = next + 1;
    }

    return false;
}

int estimate_wifi_percentage_from_dbm(int dbm)
{
    if (dbm <= -100)
    {
        return 0;
    }
    if (dbm >= -50)
    {
        return 100;
    }
    return 2 * (dbm + 100);
}

const char *wifi_signal_quality_text(int percentage)
{
    if (percentage >= 80)
    {
        return "很强";
    }
    if (percentage >= 60)
    {
        return "良好";
    }
    if (percentage >= 40)
    {
        return "一般";
    }
    if (percentage >= 20)
    {
        return "较弱";
    }
    return "很弱";
}
