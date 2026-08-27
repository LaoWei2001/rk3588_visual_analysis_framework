#ifndef FIRST_NET_CONFIG_TYPES_H
#define FIRST_NET_CONFIG_TYPES_H

#include <arpa/inet.h>
#include <stdbool.h>

#define BUF_SIZE 256
#define PROFILE_SIZE 160
#define UUID_SIZE 37
#define CMD_OUT_SIZE 4096
#define NMCLI_UP_WAIT_SEC "45"
#define PENDING_CONFIRM_TIMEOUT_SEC 120
#define ACTIVATION_ROLLBACK_TIMEOUT_SEC 180
#define IPV4_DAD_TIMEOUT_MS "3000"

typedef struct
{
    bool is_static;
    char ip[INET_ADDRSTRLEN];
    int prefix;
    char gateway[INET_ADDRSTRLEN];
    char dns[BUF_SIZE];
} IPv4Config;

typedef struct
{
    char name[PROFILE_SIZE];
    char uuid[UUID_SIZE];
} ConnectionProfile;

typedef enum
{
    NETWORK_ACTIVATION_FAILED = 0,
    NETWORK_ACTIVATION_CONFIRMED,
    NETWORK_ACTIVATION_PENDING
} NetworkActivationResult;

typedef enum
{
    CONFIG_LIFETIME_TEMPORARY = 1,
    CONFIG_LIFETIME_PERMANENT = 2
} ConfigLifetime;

typedef enum
{
    NETWORK_PROFILE_EXISTING = 0,
    NETWORK_PROFILE_PERMANENT = 1,
    NETWORK_PROFILE_TEMPORARY = 2
} NetworkProfileMode;

#endif
