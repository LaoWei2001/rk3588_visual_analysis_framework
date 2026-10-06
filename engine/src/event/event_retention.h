#pragma once

#include "third_party/json/cJSON.h"
#include <cstring>

// Keep local-only evidence and any delivery that has not completed. These records
// remain available for export, diagnosis and explicit deletion/manual retry.
inline bool event_deliveries_need_retention(const cJSON *root)
{
    const cJSON *deliveries = cJSON_IsObject(root)
        ? cJSON_GetObjectItemCaseSensitive(root, "deliveries") : nullptr;
    if (!cJSON_IsArray(deliveries) || cJSON_GetArraySize(deliveries) == 0)
        return true;
    const cJSON *delivery = nullptr;
    cJSON_ArrayForEach(delivery, deliveries)
    {
        const cJSON *status = cJSON_IsObject(delivery)
            ? cJSON_GetObjectItemCaseSensitive(delivery, "status") : nullptr;
        if (!cJSON_IsString(status) || !status->valuestring ||
            std::strcmp(status->valuestring, "delivered") != 0)
            return true;
    }
    return false;
}
