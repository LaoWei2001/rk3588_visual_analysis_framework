#define _POSIX_C_SOURCE 200809L

#include "inspection/network_quality_monitor.h"
#include "common/wifi_signal_parser.h"

#include "common/cli_io.h"
#include "common/command_runner.h"
#include "common/netconfig_types.h"
#include "inspection/network_state.h"
#include "common/nmcli_parser.h"

#include <arpa/inet.h>
#include <errno.h>
#include <net/if.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define MAX_ACTIVE_NETWORK_CONNECTIONS 32
#define NETWORK_LIST_OUTPUT_SIZE 16384
#define PING_WINDOW_SIZE 20
#define NETWORK_MONITOR_REFRESH_MS 250
#define NETWORK_PROBE_TIMEOUT_SECONDS "0.2"
#define TREND_HISTORY_SECONDS 30
#define TREND_HISTORY_CAPACITY \
    (TREND_HISTORY_SECONDS * 1000 / NETWORK_MONITOR_REFRESH_MS)
#define TREND_CHART_BUFFER_SIZE (TREND_HISTORY_CAPACITY * 4 + 1)
#define TREND_CHART_ROWS 2
#define TREND_LEVELS_PER_ROW 8

typedef enum
{
    ACTIVE_CONNECTION_WIFI = 1,
    ACTIVE_CONNECTION_ETHERNET = 2
} ActiveConnectionType;

typedef enum
{
    QUALITY_UNKNOWN = -1,
    QUALITY_EXCELLENT = 0,
    QUALITY_GOOD = 1,
    QUALITY_FAIR = 2,
    QUALITY_POOR = 3,
    QUALITY_BAD = 4
} QualityGrade;

typedef struct
{
    char name[PROFILE_SIZE];
    char uuid[UUID_SIZE];
    char device[IF_NAMESIZE];
    char ssid[WIFI_SIGNAL_SSID_SIZE];
    char gateway[INET_ADDRSTRLEN];
    ActiveConnectionType type;
} ActiveNetworkConnection;

typedef struct
{
    unsigned long long rx_errors;
    unsigned long long tx_errors;
    unsigned long long rx_dropped;
    unsigned long long tx_dropped;
} InterfaceCounters;

typedef struct
{
    bool success[PING_WINDOW_SIZE];
    double latency_ms[PING_WINDOW_SIZE];
    int count;
    int next;
} PingWindow;

typedef struct
{
    double values[TREND_HISTORY_CAPACITY];
    bool valid[TREND_HISTORY_CAPACITY];
    int count;
    int next;
} TrendHistory;

static volatile sig_atomic_t monitor_stop_requested = 0;

static void handle_monitor_stop(int signal_number)
{
    (void)signal_number;
    monitor_stop_requested = 1;
}

static ActiveConnectionType connection_type_from_text(const char *type)
{
    if (type &&
        (strcmp(type, "802-11-wireless") == 0 ||
         strcmp(type, "wifi") == 0 ||
         strcmp(type, "wireless") == 0))
    {
        return ACTIVE_CONNECTION_WIFI;
    }
    if (type &&
        (strcmp(type, "802-3-ethernet") == 0 ||
         strcmp(type, "ethernet") == 0 ||
         strcmp(type, "wired") == 0))
    {
        return ACTIVE_CONNECTION_ETHERNET;
    }
    return 0;
}

static const char *connection_type_text(ActiveConnectionType type)
{
    return type == ACTIVE_CONNECTION_WIFI ? "Wi-Fi" : "有线";
}

static void read_connection_ssid(ActiveNetworkConnection *connection)
{
    char output[WIFI_SIGNAL_SSID_SIZE] = {0};
    const char *argv[] = {
        "nmcli", "--escape", "no", "-g", "802-11-wireless.ssid",
        "connection", "show", "uuid", connection->uuid, NULL};

    if (capture_cmd(argv, output, sizeof(output)) == 0 &&
        output[0] != '\0' && strcmp(output, "--") != 0)
    {
        snprintf(connection->ssid, sizeof(connection->ssid), "%s", output);
    }
    else
    {
        snprintf(connection->ssid, sizeof(connection->ssid), "%s",
                 connection->name);
    }
}

static void read_connection_gateway(ActiveNetworkConnection *connection)
{
    char output[128] = {0};
    struct in_addr parsed;
    const char *argv[] = {
        "nmcli", "-g", "IP4.GATEWAY",
        "device", "show", connection->device, NULL};

    connection->gateway[0] = '\0';
    if (capture_cmd(argv, output, sizeof(output)) == 0)
    {
        char *newline = strchr(output, '\n');

        if (newline)
        {
            *newline = '\0';
        }
        trim_space(output);
        if (inet_pton(AF_INET, output, &parsed) == 1 &&
            strcmp(output, "0.0.0.0") != 0)
        {
            (void)inet_ntop(AF_INET, &parsed,
                            connection->gateway,
                            (socklen_t)sizeof(connection->gateway));
        }
    }
}

static int collect_active_network_connections(
    ActiveNetworkConnection *connections,
    int capacity)
{
    char output[NETWORK_LIST_OUTPUT_SIZE];
    int count = 0;
    const char *argv[] = {
        "nmcli", "-t", "--escape", "yes",
        "-f", "NAME,UUID,TYPE,DEVICE",
        "connection", "show", "--active", NULL};

    if (!connections || capacity <= 0 ||
        capture_cmd(argv, output, sizeof(output)) != 0)
    {
        return -1;
    }

    {
        char *saveptr = NULL;
        char *line = strtok_r(output, "\n", &saveptr);

        while (line && count < capacity)
        {
            char *fields[4] = {0};
            int field_count = split_nmcli_escaped_fields(line, fields, 4);
            ActiveConnectionType type = field_count == 4
                ? connection_type_from_text(fields[2])
                : 0;

            if (field_count == 4 && type != 0 &&
                fields[0][0] != '\0' &&
                strlen(fields[1]) == UUID_SIZE - 1 &&
                fields[3][0] != '\0' && strcmp(fields[3], "--") != 0)
            {
                ActiveNetworkConnection *connection = &connections[count];

                memset(connection, 0, sizeof(*connection));
                snprintf(connection->name, sizeof(connection->name), "%s",
                         fields[0]);
                snprintf(connection->uuid, sizeof(connection->uuid), "%s",
                         fields[1]);
                snprintf(connection->device, sizeof(connection->device), "%s",
                         fields[3]);
                connection->type = type;
                if (type == ACTIVE_CONNECTION_WIFI)
                {
                    read_connection_ssid(connection);
                }
                read_connection_gateway(connection);
                ++count;
            }

            line = strtok_r(NULL, "\n", &saveptr);
        }
    }

    return count;
}

static bool selected_connection_is_still_active(
    const ActiveNetworkConnection *connection)
{
    char active_uuid[UUID_SIZE] = {0};

    return get_active_connection_uuid(connection->device,
                                      active_uuid,
                                      sizeof(active_uuid)) &&
           strcmp(active_uuid, connection->uuid) == 0;
}

static bool read_access_point_sample(const char *device,
                                     WifiAccessPointSample *sample)
{
    char output[NETWORK_LIST_OUTPUT_SIZE];
    const char *argv[] = {
        "nmcli", "-t", "--escape", "yes",
        "-f", "IN-USE,SSID,BSSID,SIGNAL",
        "device", "wifi", "list", "ifname", device,
        "--rescan", "no", NULL};

    return capture_cmd(argv, output, sizeof(output)) == 0 &&
           parse_active_wifi_access_point(output, sample);
}

static bool read_iw_dbm(const char *device, int *dbm)
{
    char output[CMD_OUT_SIZE];
    const char *argv[] = {
        "iw", "dev", device, "link", NULL};

    return capture_cmd(argv, output, sizeof(output)) == 0 &&
           parse_iw_signal_dbm(output, dbm);
}

static bool iw_available(void)
{
    const char *argv[] = {
        "iw", "--version", NULL};

    return run_cmd_silent(argv) == 0;
}

static bool read_sysfs_text(const char *device,
                            const char *leaf,
                            char *out,
                            size_t out_size)
{
    char path[256];
    FILE *handle;
    int written;

    if (!device || !leaf || !out || out_size == 0)
    {
        return false;
    }
    written = snprintf(path, sizeof(path),
                       "/sys/class/net/%s/%s", device, leaf);
    if (written < 0 || (size_t)written >= sizeof(path))
    {
        return false;
    }

    handle = fopen(path, "r");
    if (!handle)
    {
        return false;
    }
    if (!fgets(out, (int)out_size, handle))
    {
        fclose(handle);
        return false;
    }
    fclose(handle);
    trim_space(out);
    return out[0] != '\0';
}

static int read_ethernet_carrier(const char *device)
{
    char value[16] = {0};

    if (!read_sysfs_text(device, "carrier", value, sizeof(value)))
    {
        return -1;
    }
    if (strcmp(value, "1") == 0)
    {
        return 1;
    }
    if (strcmp(value, "0") == 0)
    {
        return 0;
    }
    return -1;
}

static int read_ethernet_speed(const char *device)
{
    char value[32] = {0};
    char *end = NULL;
    long speed;

    if (!read_sysfs_text(device, "speed", value, sizeof(value)))
    {
        return -1;
    }
    errno = 0;
    speed = strtol(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0' ||
        speed <= 0 || speed > 1000000)
    {
        return -1;
    }
    return (int)speed;
}

static void read_ethernet_duplex(const char *device,
                                 char *out,
                                 size_t out_size)
{
    if (!read_sysfs_text(device, "duplex", out, out_size))
    {
        snprintf(out, out_size, "unknown");
    }
}

static bool read_counter(const char *device,
                         const char *name,
                         unsigned long long *value)
{
    char leaf[128];
    char text[64] = {0};
    char *end = NULL;
    unsigned long long parsed;
    int written;

    written = snprintf(leaf, sizeof(leaf), "statistics/%s", name);
    if (written < 0 || (size_t)written >= sizeof(leaf) ||
        !read_sysfs_text(device, leaf, text, sizeof(text)))
    {
        return false;
    }

    errno = 0;
    parsed = strtoull(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0')
    {
        return false;
    }
    *value = parsed;
    return true;
}

static bool read_interface_counters(const char *device,
                                    InterfaceCounters *counters)
{
    return counters &&
           read_counter(device, "rx_errors", &counters->rx_errors) &&
           read_counter(device, "tx_errors", &counters->tx_errors) &&
           read_counter(device, "rx_dropped", &counters->rx_dropped) &&
           read_counter(device, "tx_dropped", &counters->tx_dropped);
}

static unsigned long long counter_delta(unsigned long long current,
                                        unsigned long long previous)
{
    return current >= previous ? current - previous : 0;
}

static unsigned long long interface_counter_delta(
    const InterfaceCounters *current,
    const InterfaceCounters *previous)
{
    return counter_delta(current->rx_errors, previous->rx_errors) +
           counter_delta(current->tx_errors, previous->tx_errors) +
           counter_delta(current->rx_dropped, previous->rx_dropped) +
           counter_delta(current->tx_dropped, previous->tx_dropped);
}

static bool parse_ping_latency(const char *output, double *latency_ms)
{
    const char *position;
    char *end = NULL;
    double value;

    if (!output || !latency_ms)
    {
        return false;
    }
    position = strstr(output, "time=");
    if (position)
    {
        position += 5;
    }
    else
    {
        position = strstr(output, "time<");
        if (!position)
        {
            return false;
        }
        position += 5;
    }

    errno = 0;
    value = strtod(position, &end);
    if (errno != 0 || end == position || value < 0.0 || value > 60000.0)
    {
        return false;
    }
    *latency_ms = value;
    return true;
}

static bool probe_gateway(const ActiveNetworkConnection *connection,
                          double *latency_ms)
{
    char output[CMD_OUT_SIZE];
    const char *argv[] = {
        "ping", "-n", "-I", connection->device,
        "-c", "1", "-W", NETWORK_PROBE_TIMEOUT_SECONDS,
        connection->gateway, NULL};

    (void)capture_cmd(argv, output, sizeof(output));
    return parse_ping_latency(output, latency_ms);
}

static void ping_window_add(PingWindow *window,
                            bool success,
                            double latency_ms)
{
    window->success[window->next] = success;
    window->latency_ms[window->next] = latency_ms;
    window->next = (window->next + 1) % PING_WINDOW_SIZE;
    if (window->count < PING_WINDOW_SIZE)
    {
        ++window->count;
    }
}

static void trend_history_add(TrendHistory *history,
                              bool valid,
                              double value)
{
    history->valid[history->next] = valid;
    history->values[history->next] = value;
    history->next = (history->next + 1) % TREND_HISTORY_CAPACITY;
    if (history->count < TREND_HISTORY_CAPACITY)
    {
        ++history->count;
    }
}

static int trend_history_index(const TrendHistory *history,
                               int logical_index)
{
    int oldest = history->count < TREND_HISTORY_CAPACITY
                     ? 0
                     : history->next;

    return (oldest + logical_index) % TREND_HISTORY_CAPACITY;
}

static bool trend_history_range(const TrendHistory *history,
                                double *minimum,
                                double *maximum)
{
    bool found = false;

    for (int offset = 0; offset < history->count; ++offset)
    {
        int index = trend_history_index(history, offset);
        double value;

        if (!history->valid[index])
        {
            continue;
        }
        value = history->values[index];
        if (!found || value < *minimum)
        {
            *minimum = value;
        }
        if (!found || value > *maximum)
        {
            *maximum = value;
        }
        found = true;
    }
    return found;
}

static void append_trend_symbol(char *chart,
                                size_t chart_size,
                                size_t *used,
                                const char *symbol)
{
    size_t length = strlen(symbol);

    if (*used + length >= chart_size)
    {
        return;
    }
    memcpy(chart + *used, symbol, length);
    *used += length;
    chart[*used] = '\0';
}

static void build_trend_chart_row(const TrendHistory *history,
                                  int chart_columns,
                                  double scale_minimum,
                                  double scale_maximum,
                                  int chart_row,
                                  char *chart,
                                  size_t chart_size)
{
    static const char *const levels[] = {
        " ", "▁", "▂", "▃", "▄", "▅", "▆", "▇", "█"};
    int first_valid_position = TREND_HISTORY_CAPACITY - history->count;
    size_t used = 0;

    chart[0] = '\0';
    for (int column = 0; column < chart_columns; ++column)
    {
        int start = column * TREND_HISTORY_CAPACITY / chart_columns;
        int end = (column + 1) * TREND_HISTORY_CAPACITY / chart_columns;
        double total = 0.0;
        int valid_count = 0;
        int filled_levels;
        int row_base = (TREND_CHART_ROWS - chart_row - 1) *
                       TREND_LEVELS_PER_ROW;
        int row_level;

        for (int position = start; position < end; ++position)
        {
            int logical_index;
            int index;

            if (position < first_valid_position)
            {
                continue;
            }
            logical_index = position - first_valid_position;
            index = trend_history_index(history, logical_index);
            if (history->valid[index])
            {
                total += history->values[index];
                ++valid_count;
            }
        }
        if (valid_count == 0)
        {
            append_trend_symbol(chart, chart_size, &used,
                                chart_row == TREND_CHART_ROWS - 1
                                    ? "·"
                                    : " ");
            continue;
        }
        if (scale_maximum <= scale_minimum)
        {
            filled_levels = TREND_LEVELS_PER_ROW;
        }
        else
        {
            double value = total / valid_count;
            double normalized =
                (value - scale_minimum) /
                (scale_maximum - scale_minimum);

            if (normalized < 0.0)
            {
                normalized = 0.0;
            }
            if (normalized > 1.0)
            {
                normalized = 1.0;
            }
            filled_levels = 1 + (int)(
                normalized *
                (TREND_CHART_ROWS * TREND_LEVELS_PER_ROW - 1) +
                0.5);
        }
        row_level = filled_levels - row_base;
        if (row_level < 0)
        {
            row_level = 0;
        }
        if (row_level > TREND_LEVELS_PER_ROW)
        {
            row_level = TREND_LEVELS_PER_ROW;
        }
        append_trend_symbol(chart, chart_size, &used, levels[row_level]);
    }
}

static void print_trend_chart(const char *label,
                              const char *unit,
                              const TrendHistory *history,
                              int visible_points,
                              bool fixed_scale,
                              double fixed_minimum,
                              double fixed_maximum,
                              bool decimal_range)
{
    char chart_rows[TREND_CHART_ROWS][TREND_CHART_BUFFER_SIZE];
    double observed_minimum = 0.0;
    double observed_maximum = 0.0;
    double scale_minimum;
    double scale_maximum;

    if (!trend_history_range(history,
                             &observed_minimum, &observed_maximum))
    {
        printf("%s：暂无数据\n", label);
        return;
    }
    scale_minimum = fixed_scale ? fixed_minimum : observed_minimum;
    scale_maximum = fixed_scale ? fixed_maximum : observed_maximum;
    if (decimal_range)
    {
        printf("%s：%.1f~%.1f%s\n", label,
               observed_minimum, observed_maximum, unit);
    }
    else
    {
        printf("%s：%.0f~%.0f%s\n", label,
               observed_minimum, observed_maximum, unit);
    }
    for (int row = 0; row < TREND_CHART_ROWS; ++row)
    {
        build_trend_chart_row(history, visible_points,
                              scale_minimum, scale_maximum,
                              row, chart_rows[row],
                              sizeof(chart_rows[row]));
        if (decimal_range)
        {
            printf(" %7.1f ┤%s\n",
                   row == 0 ? scale_maximum : scale_minimum,
                   chart_rows[row]);
        }
        else
        {
            printf(" %7.0f ┤%s\n",
                   row == 0 ? scale_maximum : scale_minimum,
                   chart_rows[row]);
        }
    }
}

static bool ping_window_summary(const PingWindow *window,
                                int *loss_percent,
                                double *average_latency_ms,
                                int *success_count)
{
    int successes = 0;
    double latency_total = 0.0;

    if (!window || window->count <= 0)
    {
        return false;
    }

    for (int index = 0; index < window->count; ++index)
    {
        if (window->success[index])
        {
            ++successes;
            latency_total += window->latency_ms[index];
        }
    }

    *loss_percent = ((window->count - successes) * 100 +
                     window->count / 2) / window->count;
    *success_count = successes;
    *average_latency_ms = successes > 0
        ? latency_total / successes
        : 0.0;
    return true;
}

static QualityGrade worse_quality(QualityGrade left, QualityGrade right)
{
    if (left == QUALITY_UNKNOWN)
    {
        return right;
    }
    if (right == QUALITY_UNKNOWN)
    {
        return left;
    }
    return left > right ? left : right;
}

static QualityGrade wifi_quality(bool has_sample,
                                 const WifiAccessPointSample *sample,
                                 bool has_dbm,
                                 int dbm)
{
    int percentage;

    if (has_sample && sample->percentage >= 0)
    {
        percentage = sample->percentage;
    }
    else if (has_dbm)
    {
        percentage = estimate_wifi_percentage_from_dbm(dbm);
    }
    else
    {
        return QUALITY_UNKNOWN;
    }

    if (percentage >= 80)
    {
        return QUALITY_EXCELLENT;
    }
    if (percentage >= 60)
    {
        return QUALITY_GOOD;
    }
    if (percentage >= 40)
    {
        return QUALITY_FAIR;
    }
    if (percentage >= 20)
    {
        return QUALITY_POOR;
    }
    return QUALITY_BAD;
}

static QualityGrade ethernet_quality(int carrier,
                                     const char *duplex,
                                     bool has_counter_delta,
                                     unsigned long long error_delta)
{
    QualityGrade grade;

    if (carrier == 0)
    {
        return QUALITY_BAD;
    }
    if (carrier < 0)
    {
        return QUALITY_UNKNOWN;
    }

    grade = QUALITY_EXCELLENT;
    if (duplex && strcmp(duplex, "half") == 0)
    {
        grade = QUALITY_FAIR;
    }
    if (has_counter_delta && error_delta > 0)
    {
        grade = worse_quality(
            grade,
            error_delta >= 10 ? QUALITY_POOR : QUALITY_FAIR);
    }
    return grade;
}

static QualityGrade gateway_quality(const PingWindow *window)
{
    int loss_percent;
    int success_count;
    double latency_ms;
    QualityGrade grade = QUALITY_EXCELLENT;

    if (!ping_window_summary(window,
                             &loss_percent,
                             &latency_ms,
                             &success_count))
    {
        return QUALITY_UNKNOWN;
    }
    if (loss_percent >= 50)
    {
        return QUALITY_BAD;
    }
    if (loss_percent >= 20)
    {
        grade = QUALITY_POOR;
    }
    else if (loss_percent > 0)
    {
        grade = QUALITY_FAIR;
    }

    if (success_count > 0)
    {
        if (latency_ms >= 200.0)
        {
            grade = worse_quality(grade, QUALITY_POOR);
        }
        else if (latency_ms >= 100.0)
        {
            grade = worse_quality(grade, QUALITY_FAIR);
        }
        else if (latency_ms >= 50.0)
        {
            grade = worse_quality(grade, QUALITY_GOOD);
        }
    }
    return grade;
}

static const char *quality_text(QualityGrade grade)
{
    switch (grade)
    {
    case QUALITY_EXCELLENT:
        return "优秀";
    case QUALITY_GOOD:
        return "良好";
    case QUALITY_FAIR:
        return "一般";
    case QUALITY_POOR:
        return "较差";
    case QUALITY_BAD:
        return "很差";
    default:
        return "暂无法评估";
    }
}

static const char *duplex_text(const char *duplex)
{
    if (duplex && strcmp(duplex, "full") == 0)
    {
        return "全双工";
    }
    if (duplex && strcmp(duplex, "half") == 0)
    {
        return "半双工";
    }
    return "双工未知";
}

static void current_time_text(char *out, size_t out_size)
{
    struct timespec now;
    struct tm local_time;
    char seconds[16];

    if (clock_gettime(CLOCK_REALTIME, &now) == 0 &&
        localtime_r(&now.tv_sec, &local_time) &&
        strftime(seconds, sizeof(seconds), "%H:%M:%S", &local_time) > 0 &&
        snprintf(out, out_size, "%s.%03ld", seconds,
                 now.tv_nsec / (1000L * 1000L)) > 0)
    {
        return;
    }
    snprintf(out, out_size, "--:--:--.---");
}

static int compare_timespec(const struct timespec *left,
                            const struct timespec *right)
{
    if (left->tv_sec != right->tv_sec)
    {
        return left->tv_sec < right->tv_sec ? -1 : 1;
    }
    if (left->tv_nsec == right->tv_nsec)
    {
        return 0;
    }
    return left->tv_nsec < right->tv_nsec ? -1 : 1;
}

static void wait_for_next_refresh(struct timespec *deadline)
{
    struct timespec now;

    deadline->tv_nsec += NETWORK_MONITOR_REFRESH_MS * 1000L * 1000L;
    while (deadline->tv_nsec >= 1000L * 1000L * 1000L)
    {
        deadline->tv_nsec -= 1000L * 1000L * 1000L;
        ++deadline->tv_sec;
    }
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
    {
        const struct timespec fallback = {
            .tv_sec = 0,
            .tv_nsec = NETWORK_MONITOR_REFRESH_MS * 1000L * 1000L};
        (void)nanosleep(&fallback, NULL);
        return;
    }
    if (compare_timespec(deadline, &now) <= 0)
    {
        *deadline = now;
        return;
    }
    while (!monitor_stop_requested &&
           clock_nanosleep(CLOCK_MONOTONIC,
                           TIMER_ABSTIME,
                           deadline,
                           NULL) == EINTR)
    {
    }
}

static void print_gateway_metrics(const ActiveNetworkConnection *connection,
                                  const PingWindow *window,
                                  int *loss_percent_out)
{
    int loss_percent;
    int success_count;
    double latency_ms;

    *loss_percent_out = -1;
    if (connection->gateway[0] == '\0')
    {
        printf("%s路由器：未配置",
               terminal_ui_enabled() ? "" : " | ");
        return;
    }
    if (!ping_window_summary(window,
                             &loss_percent,
                             &latency_ms,
                             &success_count))
    {
        printf("%s路由器：等待探测",
               terminal_ui_enabled() ? "" : " | ");
        return;
    }

    *loss_percent_out = loss_percent;
    printf("%s路由器丢包：%d%%（%d次）",
           terminal_ui_enabled() ? "" : " | ",
           loss_percent, window->count);
    if (success_count > 0)
    {
        printf("  延迟：%.1fms", latency_ms);
    }
    else
    {
        printf("  延迟：--");
    }
}

static void print_wifi_metrics(bool has_sample,
                               const WifiAccessPointSample *sample,
                               bool has_dbm,
                               int dbm,
                               bool has_signal_trend,
                               int signal_change,
                               bool access_point_changed)
{
    int percentage = has_sample ? sample->percentage : -1;
    bool estimated = false;

    if (percentage < 0 && has_dbm)
    {
        percentage = estimate_wifi_percentage_from_dbm(dbm);
        estimated = true;
    }

    printf("%sWi-Fi：", terminal_ui_enabled() ? "" : " | ");
    if (percentage >= 0)
    {
        printf("%d%%%s", percentage, estimated ? "(估算)" : "");
    }
    else
    {
        printf("强度未知");
    }
    if (has_dbm)
    {
        printf("/%d dBm", dbm);
    }
    if (has_signal_trend)
    {
        if (signal_change > 0)
        {
            printf(" 增强%+d%s", signal_change, has_dbm ? "dBm" : "%");
        }
        else if (signal_change < 0)
        {
            printf(" 减弱%+d%s", signal_change, has_dbm ? "dBm" : "%");
        }
        else
        {
            printf(" 稳定");
        }
    }
    if (access_point_changed)
    {
        printf(" [接入点已切换]");
    }
}

static void print_ethernet_metrics(int carrier,
                                   int speed,
                                   const char *duplex,
                                   bool has_counter_delta,
                                   unsigned long long error_delta)
{
    if (carrier == 0)
    {
        printf("%s网线：未接", terminal_ui_enabled() ? "" : " | ");
    }
    else if (carrier < 0)
    {
        printf("%s网线：未知", terminal_ui_enabled() ? "" : " | ");
    }
    else
    {
        printf("%s网线：已接", terminal_ui_enabled() ? "" : " | ");
        if (speed > 0)
        {
            printf(" %dMb/s", speed);
        }
        printf(" %s", duplex_text(duplex));
    }
    if (has_counter_delta)
    {
        printf("%s错误/丢弃包：+%llu",
               terminal_ui_enabled() ? "  " : " | ", error_delta);
    }
}

static void run_quality_monitor(const ActiveNetworkConnection *connection)
{
    struct sigaction action;
    struct sigaction previous_action;
    struct timespec next_refresh = {0};
    PingWindow ping_window = {0};
    TrendHistory signal_history = {0};
    TrendHistory latency_history = {0};
    TrendHistory loss_history = {0};
    InterfaceCounters previous_counters = {0};
    bool restore_signal_action = false;
    bool stdout_is_terminal = isatty(STDOUT_FILENO) != 0;
    bool use_iw = connection->type == ACTIVE_CONNECTION_WIFI && iw_available();
    bool has_previous_counters = false;
    bool has_previous_signal = false;
    bool previous_signal_is_dbm = false;
    int previous_signal = 0;
    char previous_bssid[WIFI_SIGNAL_BSSID_SIZE] = {0};

    monitor_stop_requested = 0;
    memset(&action, 0, sizeof(action));
    action.sa_handler = handle_monitor_stop;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGINT, &action, &previous_action) == 0)
    {
        restore_signal_action = true;
    }

    if (connection->type == ACTIVE_CONNECTION_ETHERNET)
    {
        has_previous_counters = read_interface_counters(
            connection->device, &previous_counters);
    }

    if (terminal_ui_enabled())
    {
        terminal_ui_begin_screen("实时连接质量");
        printf("\n========== 实时网络质量 ==========\n");
        printf("网络：%s（%s / %s）\n",
               connection->type == ACTIVE_CONNECTION_WIFI
                   ? connection->ssid
                   : connection->name,
               connection_type_text(connection->type),
               connection->device);
        printf("路由器：%s\n",
               connection->gateway[0]
                   ? connection->gateway
                   : "未配置，仅检测网卡连接");
        printf("曲线最多显示最近约 %d 秒，左侧较早，右侧最新。\n",
               TREND_HISTORY_SECONDS);
        if (connection->gateway[0])
        {
            printf("[提示] 路由器不响应探测时，丢包高不一定代表不能联网。\n");
        }
    }
    else
    {
        printf("\n========== 实时网络质量监测 ==========\n");
        printf("连接：%s\n", connection->name);
        printf("连接方式：%s  网卡：%s\n",
               connection_type_text(connection->type), connection->device);
        if (connection->type == ACTIVE_CONNECTION_WIFI)
        {
            printf("网络名称（SSID）：%s\n", connection->ssid);
        }
        printf("质量探测目标：%s\n",
               connection->gateway[0]
                   ? connection->gateway
                   : "未配置网关，仅检测链路");
        if (connection->gateway[0])
        {
            printf("每 %.1f 秒刷新，丢包按最近 %d 次滚动统计；Ctrl+C 返回。\n",
                   NETWORK_MONITOR_REFRESH_MS / 1000.0,
                   PING_WINDOW_SIZE);
            printf("网关若禁用 ICMP，100%% 丢包仅表示未响应探测。\n");
        }
        else
        {
            printf("此连接没有 IPv4 网关，已跳过丢包和延迟检测；"
                   "每 %.1f 秒刷新；按 Ctrl+C 返回主菜单。\n",
                   NETWORK_MONITOR_REFRESH_MS / 1000.0);
        }
    }
    if (connection->type == ACTIVE_CONNECTION_WIFI && !use_iw)
    {
        printf("[提示] 将使用系统提供的 Wi-Fi 信号强度。\n");
    }
    if (terminal_ui_enabled())
    {
        terminal_ui_set_footer("[ 停止监测并返回网络选择 ]   Esc/Q");
        terminal_ui_mark_live_region();
    }
    (void)clock_gettime(CLOCK_MONOTONIC, &next_refresh);

    while (!monitor_stop_requested)
    {
        WifiAccessPointSample wifi_sample;
        InterfaceCounters current_counters = {0};
        QualityGrade link_quality = QUALITY_UNKNOWN;
        QualityGrade probe_quality = QUALITY_UNKNOWN;
        QualityGrade overall_quality;
        bool has_wifi_sample = false;
        bool has_dbm = false;
        bool has_counter_delta = false;
        bool has_signal_trend = false;
        bool access_point_changed = false;
        int carrier = -1;
        int speed = -1;
        int dbm = 0;
        int signal_change = 0;
        int signal_percentage = -1;
        int loss_percent = -1;
        bool has_latest_latency = false;
        double latest_latency_ms = 0.0;
        unsigned long long error_delta = 0;
        char duplex[32] = "unknown";
        char time_text[16];

        if (!selected_connection_is_still_active(connection))
        {
            if (monitor_stop_requested)
            {
                break;
            }
            if (terminal_ui_enabled())
            {
                terminal_ui_reset_live_region();
            }
            else if (stdout_is_terminal)
            {
                printf("\r\033[2K");
            }
            printf("[监测结束] 所选网络已停用，或已切换为其他连接。\n");
            break;
        }

        memset(&wifi_sample, 0, sizeof(wifi_sample));
        wifi_sample.percentage = -1;
        if (connection->type == ACTIVE_CONNECTION_WIFI)
        {
            int current_signal;
            bool current_signal_is_dbm;

            has_wifi_sample = read_access_point_sample(
                connection->device, &wifi_sample);
            if (monitor_stop_requested)
            {
                break;
            }
            if (use_iw)
            {
                has_dbm = read_iw_dbm(connection->device, &dbm);
            }
            if (monitor_stop_requested)
            {
                break;
            }

            if (wifi_sample.bssid[0] != '\0')
            {
                access_point_changed = previous_bssid[0] != '\0' &&
                    strcmp(previous_bssid, wifi_sample.bssid) != 0;
                snprintf(previous_bssid, sizeof(previous_bssid), "%s",
                         wifi_sample.bssid);
            }

            current_signal_is_dbm = has_dbm;
            current_signal = has_dbm ? dbm : wifi_sample.percentage;
            if (has_dbm || wifi_sample.percentage >= 0)
            {
                has_signal_trend = has_previous_signal &&
                    previous_signal_is_dbm == current_signal_is_dbm;
                signal_change = current_signal - previous_signal;
                previous_signal = current_signal;
                previous_signal_is_dbm = current_signal_is_dbm;
                has_previous_signal = true;
            }
            link_quality = wifi_quality(has_wifi_sample,
                                        &wifi_sample,
                                        has_dbm,
                                        dbm);
            signal_percentage = has_wifi_sample
                                    ? wifi_sample.percentage
                                    : -1;
            if (signal_percentage < 0 && has_dbm)
            {
                signal_percentage = estimate_wifi_percentage_from_dbm(dbm);
            }
        }
        else
        {
            bool current_counters_valid;

            carrier = read_ethernet_carrier(connection->device);
            speed = read_ethernet_speed(connection->device);
            read_ethernet_duplex(connection->device,
                                 duplex, sizeof(duplex));
            current_counters_valid = read_interface_counters(
                connection->device, &current_counters);
            has_counter_delta = has_previous_counters &&
                                current_counters_valid;
            if (has_counter_delta)
            {
                error_delta = interface_counter_delta(
                    &current_counters, &previous_counters);
            }
            if (current_counters_valid)
            {
                previous_counters = current_counters;
                has_previous_counters = true;
            }
            link_quality = ethernet_quality(carrier,
                                            duplex,
                                            has_counter_delta,
                                            error_delta);
        }

        if (connection->gateway[0] != '\0')
        {
            has_latest_latency = probe_gateway(connection,
                                               &latest_latency_ms);

            if (monitor_stop_requested)
            {
                break;
            }
            ping_window_add(&ping_window,
                            has_latest_latency,
                            latest_latency_ms);
            probe_quality = gateway_quality(&ping_window);
        }

        overall_quality = worse_quality(link_quality, probe_quality);
        current_time_text(time_text, sizeof(time_text));
        if (terminal_ui_enabled())
        {
            terminal_ui_begin_update();
            terminal_ui_reset_live_region();
        }
        else if (stdout_is_terminal)
        {
            printf("\r\033[2K");
        }
        printf("[%s] 质量：%s", time_text, quality_text(overall_quality));
        if (terminal_ui_enabled())
        {
            printf("\n");
        }
        if (connection->type == ACTIVE_CONNECTION_WIFI)
        {
            print_wifi_metrics(has_wifi_sample,
                               &wifi_sample,
                               has_dbm,
                               dbm,
                               has_signal_trend,
                               signal_change,
                               access_point_changed);
        }
        else
        {
            print_ethernet_metrics(carrier,
                                   speed,
                                   duplex,
                                   has_counter_delta,
                                   error_delta);
        }
        if (terminal_ui_enabled())
        {
            printf("\n");
        }
        print_gateway_metrics(connection, &ping_window, &loss_percent);
        if (connection->type == ACTIVE_CONNECTION_WIFI)
        {
            if (signal_percentage >= 0 && signal_percentage < 30)
            {
                printf("%s[信号弱]",
                       terminal_ui_enabled() ? "  " : " | ");
            }
        }
        else if (carrier == 0)
        {
            printf("%s[请检查网线或对端设备]",
                   terminal_ui_enabled() ? "  " : " | ");
        }
        if (loss_percent >= 20)
        {
            printf("%s[丢包偏高]",
                   terminal_ui_enabled() ? "  " : " | ");
        }
        if (terminal_ui_enabled())
        {
            int chart_points = terminal_ui_content_width() - 12;

            if (chart_points < 12)
            {
                chart_points = 12;
            }
            if (chart_points > TREND_HISTORY_CAPACITY)
            {
                chart_points = TREND_HISTORY_CAPACITY;
            }
            trend_history_add(&signal_history,
                              signal_percentage >= 0,
                              signal_percentage);
            trend_history_add(&latency_history,
                              has_latest_latency,
                              latest_latency_ms);
            trend_history_add(&loss_history,
                              loss_percent >= 0,
                              loss_percent);
            printf("\n\n========== 历史趋势（左旧右新） ==========\n");
            if (connection->type == ACTIVE_CONNECTION_WIFI)
            {
                print_trend_chart("信号", "%", &signal_history,
                                  chart_points, false, 0.0, 0.0, false);
                printf("\n\n");
            }
            if (connection->gateway[0])
            {
                print_trend_chart("延迟", "ms", &latency_history,
                                  chart_points, false, 0.0, 0.0, true);
                printf("\n\n");
                print_trend_chart("丢包", "%", &loss_history,
                                  chart_points, true, 0.0, 100.0, false);
            }
        }
        if (!stdout_is_terminal || terminal_ui_enabled())
        {
            printf("\n");
        }
        if (terminal_ui_enabled())
        {
            terminal_ui_end_update();
        }
        fflush(stdout);

        if (terminal_ui_poll_cancel())
        {
            monitor_stop_requested = 1;
        }

        if (!monitor_stop_requested)
        {
            wait_for_next_refresh(&next_refresh);
        }
    }

    if (stdout_is_terminal && !terminal_ui_enabled())
    {
        printf("\r\033[2K");
    }
    if (monitor_stop_requested)
    {
        printf("实时监测已停止。\n");
    }
    if (restore_signal_action)
    {
        (void)sigaction(SIGINT, &previous_action, NULL);
    }
}

static void print_connection_choice(int index,
                                    const ActiveNetworkConnection *connection)
{
    printf("  %d. [%s] %s  网卡: %s",
           index,
           connection_type_text(connection->type),
           connection->type == ACTIVE_CONNECTION_WIFI
               ? connection->ssid
               : connection->name,
           connection->device);

    if (connection->type == ACTIVE_CONNECTION_WIFI)
    {
        WifiAccessPointSample sample;

        if (read_access_point_sample(connection->device, &sample) &&
            sample.percentage >= 0)
        {
            printf("  当前信号: %d%%（%s）",
                   sample.percentage,
                   wifi_signal_quality_text(sample.percentage));
        }
    }
    else
    {
        int carrier = read_ethernet_carrier(connection->device);
        int speed = read_ethernet_speed(connection->device);

        if (carrier == 1)
        {
            printf("  物理链路: 已接通");
            if (speed > 0)
            {
                printf(" %dMb/s", speed);
            }
        }
        else if (carrier == 0)
        {
            printf("  物理链路: 未接通");
        }
        else
        {
            printf("  物理链路: 未知");
        }
    }
    printf("\n");
    printf("     连接配置: %s  网关: %s\n",
           connection->name,
           connection->gateway[0] ? connection->gateway : "未配置");
}

void monitor_active_network_quality(void)
{
    for (;;)
    {
        ActiveNetworkConnection connections[MAX_ACTIVE_NETWORK_CONNECTIONS] = {0};
        int connection_count;
        int choice;

        /* 每次从曲线返回时刷新列表，避免继续显示已经停用的连接。 */
        if (terminal_ui_enabled())
        {
            terminal_ui_begin_screen("实时质量曲线");
        }
        connection_count = collect_active_network_connections(
            connections, MAX_ACTIVE_NETWORK_CONNECTIONS);

        printf("\n========== 选择已启用的网络 ==========\n");
        if (connection_count < 0)
        {
            printf("无法读取当前活动的网络连接，请稍后重试。\n");
            return;
        }
        if (connection_count == 0)
        {
            printf("当前没有已启用的有线或 Wi-Fi 网络。\n");
            return;
        }

        printf("程序将按连接类型自动选用对应质量指标。\n");
        for (int index = 0; index < connection_count; ++index)
        {
            print_connection_choice(index + 1, &connections[index]);
        }
        printf("  0. 返回主菜单\n");

        terminal_ui_set_step("选择监测网络",
                             "选择要查看实时质量曲线的活动连接");
        choice = read_int("请选择要实时监测的网络: ",
                          0,
                          connection_count);
        if (choice == TERMINAL_UI_INPUT_CANCELLED)
        {
            return;
        }
        if (choice == 0)
        {
            return;
        }
        run_quality_monitor(&connections[choice - 1]);
        if (!terminal_ui_enabled())
        {
            return;
        }
    }
}
