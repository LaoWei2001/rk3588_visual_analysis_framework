#include "common/nmcli_parser.h"

int split_nmcli_escaped_fields(char *line, char *fields[], int capacity)
{
    char *source = line;
    char *destination = line;
    int count = 0;

    if (!line || !fields || capacity <= 0)
    {
        return 0;
    }

    fields[count++] = destination;
    while (*source)
    {
        if (*source == '\\' && source[1] != '\0')
        {
            ++source;
            *destination++ = *source++;
            continue;
        }
        if (*source == ':' && count < capacity)
        {
            *destination++ = '\0';
            ++source;
            fields[count++] = destination;
            continue;
        }
        *destination++ = *source++;
    }
    *destination = '\0';
    return count;
}
