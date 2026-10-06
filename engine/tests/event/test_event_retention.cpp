#include "event/event_retention.h"
#include <cassert>
#include <string>

int main()
{
    for (const char *status : {"pending", "retry", "uploading", "failed", "invalid", "unknown", ""})
    {
        const std::string text = "{\"deliveries\":[{\"status\":\"" + std::string(status) + "\"}]}";
        cJSON *root = cJSON_Parse(text.c_str());
        assert(event_deliveries_need_retention(root));
        cJSON_Delete(root);
    }
    for (const char *text : {"{}", "{\"deliveries\":[]}", "{\"deliveries\":null}", "{\"deliveries\":[null]}",
                             "{\"deliveries\":[{}]}",
                             "{\"deliveries\":[{\"status\":\"delivered\"},{\"status\":\"retry\"}]}"})
    {
        cJSON *root = cJSON_Parse(text);
        assert(event_deliveries_need_retention(root));
        cJSON_Delete(root);
    }
    for (const char *text : {"{\"deliveries\":[{\"status\":\"delivered\"}]}"})
    {
        cJSON *root = cJSON_Parse(text);
        assert(!event_deliveries_need_retention(root));
        cJSON_Delete(root);
    }
    assert(event_deliveries_need_retention(nullptr));
}
