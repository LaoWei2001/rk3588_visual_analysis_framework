#include "logic/context_access.h"
#include <events.h>

namespace
{
EventReportResult invalid_submission(const char *detail)
{
    EventReportResult result;
    result.status = EventReportStatus::INVALID_REQUEST;
    result.detail = detail;
    return result;
}
} // namespace

EventReportResult EventSink::report(GlobalContext &, const EventRequest &)
{
    return invalid_submission("event sink does not support global events");
}

EventReportResult report_event(ChannelContext *context, const EventRequest &request)
{
    if (!context || request.event_type.empty())
        return invalid_submission("channel context is null or event type is empty");
    EventSink *sink = VisionContextAccess::events(*context);
    if (!sink)
        return invalid_submission("channel session has no event sink");
    return sink->report(*context, request);
}

EventReportResult report_event(GlobalContext *context, const EventRequest &request)
{
    if (!context || request.event_type.empty())
        return invalid_submission("global context is null or event type is empty");
    EventSink *sink = VisionContextAccess::events(*context);
    if (!sink)
        return invalid_submission("global session has no event sink");
    return sink->report(*context, request);
}

const char *event_report_status_name(EventReportStatus status)
{
    switch (status)
    {
    case EventReportStatus::CREATED:
        return "created";
    case EventReportStatus::MERGED:
        return "merged";
    case EventReportStatus::CREATED_MEDIA_FAILED:
        return "created_media_failed";
    case EventReportStatus::DISABLED:
        return "disabled";
    case EventReportStatus::INVALID_REQUEST:
        return "invalid_request";
    case EventReportStatus::NO_DELIVERY:
        return "no_delivery";
    case EventReportStatus::WORKER_UNAVAILABLE:
        return "worker_unavailable";
    case EventReportStatus::STORAGE_ERROR:
        return "storage_error";
    }
    return "unknown";
}
