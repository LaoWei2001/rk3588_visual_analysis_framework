#include "event_report_internal.h"
#include <algorithm>
#include <cstdio>
#include <cstring>

namespace event_report_detail
{
cJSON *fields_to_json(const EventFields &fields)
{
    cJSON *obj = cJSON_CreateObject();
    for (const auto &entry : fields.values())
    {
        const EventValue &v = entry.second;
        if (v.type() == EventValue::NUMBER)
            cJSON_AddNumberToObject(obj, entry.first.c_str(), v.number());
        else if (v.type() == EventValue::BOOLEAN)
            cJSON_AddBoolToObject(obj, entry.first.c_str(), v.boolean() ? 1 : 0);
        else if (v.type() == EventValue::JSON)
        {
            cJSON *parsed = cJSON_Parse(v.text().c_str());
            if (parsed)
                cJSON_AddItemToObject(obj, entry.first.c_str(), parsed);
            else
                cJSON_AddStringToObject(obj, entry.first.c_str(), v.text().c_str());
        }
        else
            cJSON_AddStringToObject(obj, entry.first.c_str(), v.text().c_str());
    }
    return obj;
}

cJSON *parse_object_or_empty(const std::string &text)
{
    cJSON *obj = cJSON_Parse(text.c_str());
    if (!cJSON_IsObject(obj))
    {
        cJSON_Delete(obj);
        obj = cJSON_CreateObject();
    }
    return obj;
}

bool delivery_matches_event(const cJSON *delivery, const std::string &event_type)
{
    const cJSON *when = cJSON_GetObjectItemCaseSensitive(delivery, "when");
    if (!cJSON_IsObject(when))
        return true;
    const cJSON *types = cJSON_GetObjectItemCaseSensitive(when, "event_types");
    if (!cJSON_IsArray(types) || cJSON_GetArraySize(types) == 0)
        return true;
    const cJSON *item = nullptr;
    cJSON_ArrayForEach(item, types)
    {
        if (cJSON_IsString(item) && item->valuestring && event_type == item->valuestring)
            return true;
    }
    return false;
}

bool delivery_needs_video(cJSON *policy, const std::string &event_type)
{
    cJSON *deliveries = cJSON_GetObjectItemCaseSensitive(policy, "deliveries");
    cJSON *delivery = nullptr;
    cJSON_ArrayForEach(delivery, deliveries)
    {
        const cJSON *enabled = cJSON_GetObjectItemCaseSensitive(delivery, "enabled");
        const cJSON *connection_id = cJSON_GetObjectItemCaseSensitive(delivery, "connection_id");
        const cJSON *contract_id = cJSON_GetObjectItemCaseSensitive(delivery, "contract_id");
        const cJSON *contract_revision = cJSON_GetObjectItemCaseSensitive(delivery, "contract_revision");
        const cJSON *media = cJSON_GetObjectItemCaseSensitive(delivery, "media");
        if (cJSON_IsFalse(enabled) || !cJSON_IsString(connection_id) || !connection_id->valuestring[0] ||
            !cJSON_IsString(contract_id) || !contract_id->valuestring[0] || !cJSON_IsString(contract_revision) ||
            !contract_revision->valuestring[0] || !cJSON_IsArray(media) ||
            !delivery_matches_event(delivery, event_type))
            continue;

        const cJSON *kind = nullptr;
        cJSON_ArrayForEach(kind, media)
        {
            if (cJSON_IsString(kind) && kind->valuestring && strcmp(kind->valuestring, "video") == 0)
                return true;
        }
    }
    return false;
}

void add_delivery_statuses(cJSON *policy, const std::string &event_type, cJSON *root)
{
    cJSON *out = cJSON_CreateArray();
    cJSON *arr = cJSON_GetObjectItemCaseSensitive(policy, "deliveries");
    cJSON *item = nullptr;
    if (cJSON_IsArray(arr))
    {
        int index = 0;
        cJSON_ArrayForEach(item, arr)
        {
            cJSON *enabled = cJSON_GetObjectItemCaseSensitive(item, "enabled");
            cJSON *connection_id = cJSON_GetObjectItemCaseSensitive(item, "connection_id");
            cJSON *contract_id = cJSON_GetObjectItemCaseSensitive(item, "contract_id");
            cJSON *contract_revision = cJSON_GetObjectItemCaseSensitive(item, "contract_revision");
            cJSON *media = cJSON_GetObjectItemCaseSensitive(item, "media");
            if (cJSON_IsFalse(enabled) || !cJSON_IsString(connection_id) || !connection_id->valuestring[0] ||
                !cJSON_IsString(contract_id) || !contract_id->valuestring[0] || !cJSON_IsString(contract_revision) ||
                !contract_revision->valuestring[0] || !cJSON_IsArray(media) ||
                !delivery_matches_event(item, event_type))
            {
                ++index;
                continue;
            }
            cJSON *copy = cJSON_Duplicate(item, 1);
            cJSON *id = cJSON_GetObjectItemCaseSensitive(copy, "id");
            if (!cJSON_IsString(id))
            {
                char generated[32];
                snprintf(generated, sizeof(generated), "delivery_%d", index);
                cJSON_AddStringToObject(copy, "id", generated);
            }
            cJSON_AddStringToObject(copy, "status", "pending");
            cJSON_AddNumberToObject(copy, "attempts", 0);
            cJSON_AddStringToObject(copy, "last_error", "");
            cJSON_AddItemToArray(out, copy);
            ++index;
        }
    }
    cJSON_AddItemToObject(root, "deliveries", out);
}

double policy_number(cJSON *policy, const char *key, double fallback)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(policy, key);
    return cJSON_IsNumber(item) ? item->valuedouble : fallback;
}

std::string policy_string(cJSON *policy, const char *key, const char *fallback)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(policy, key);
    return cJSON_IsString(item) ? item->valuestring : fallback;
}

void append_unique_channel(std::vector<int> &channels, int channel_id)
{
    if (channel_id >= 0 && std::find(channels.begin(), channels.end(), channel_id) == channels.end())
        channels.push_back(channel_id);
}

cJSON *channel_ids_to_json(const std::vector<int> &channels)
{
    cJSON *array = cJSON_CreateArray();
    for (int channel_id : channels)
        cJSON_AddItemToArray(array, cJSON_CreateNumber(channel_id));
    return array;
}

bool parse_image_selection(cJSON *policy, ImageSelectionPolicy *out, std::string *error)
{
    if (!out)
        return false;
    *out = ImageSelectionPolicy();
    cJSON *selection = cJSON_GetObjectItemCaseSensitive(policy, "image_selection");
    if (!selection)
        return true; /* 旧配置继续使用全部画布输入。 */
    if (!cJSON_IsObject(selection))
    {
        if (error)
            *error = "report policy image_selection must be an object";
        return false;
    }

    cJSON *mode = cJSON_GetObjectItemCaseSensitive(selection, "mode");
    if (cJSON_IsString(mode) && mode->valuestring && mode->valuestring[0])
        out->mode = mode->valuestring;
    if (out->mode != "connected" && out->mode != "selected" && out->mode != "event_evidence")
    {
        if (error)
            *error = "report policy image_selection.mode is invalid";
        return false;
    }

    cJSON *channel_ids = cJSON_GetObjectItemCaseSensitive(selection, "channel_ids");
    if (channel_ids && !cJSON_IsArray(channel_ids))
    {
        if (error)
            *error = "report policy image_selection.channel_ids must be an array";
        return false;
    }
    cJSON *item = nullptr;
    cJSON_ArrayForEach(item, channel_ids)
    {
        if (!cJSON_IsNumber(item) || item->valuedouble != static_cast<double>(item->valueint))
        {
            if (error)
                *error = "report policy image_selection.channel_ids must contain integers";
            return false;
        }
        append_unique_channel(out->channel_ids, item->valueint);
    }
    return true;
}
} // namespace event_report_detail
