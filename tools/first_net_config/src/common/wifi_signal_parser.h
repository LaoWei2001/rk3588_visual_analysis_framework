#ifndef FIRST_NET_CONFIG_WIFI_SIGNAL_PARSER_H
#define FIRST_NET_CONFIG_WIFI_SIGNAL_PARSER_H

#include <stdbool.h>

#define WIFI_SIGNAL_SSID_SIZE 256
#define WIFI_SIGNAL_BSSID_SIZE 32

typedef struct
{
    char ssid[WIFI_SIGNAL_SSID_SIZE];
    char bssid[WIFI_SIGNAL_BSSID_SIZE];
    int percentage;
} WifiAccessPointSample;

bool parse_active_wifi_access_point(char *output,
                                    WifiAccessPointSample *sample);
bool parse_iw_signal_dbm(const char *output, int *dbm);
int estimate_wifi_percentage_from_dbm(int dbm);
const char *wifi_signal_quality_text(int percentage);

#endif
