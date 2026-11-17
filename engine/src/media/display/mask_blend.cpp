#include <drawing.h>
#include <opencv2/imgproc.hpp>

bool vision::blend_mask(cv::Mat &canvas, const cv::Mat &mask, const cv::Scalar &color, double alpha)
{
    if (mask.empty() || mask.type() != CV_8UC1 || alpha <= 0.0)
        return false;

    if (canvas.empty() || canvas.type() != CV_8UC3)
        return false;

    struct BlendScratch
    {
        cv::Mat resized_mask;
        cv::Mat solid;
        cv::Mat blended;
        cv::Scalar solid_color;
        bool solid_valid = false;
    };
    thread_local BlendScratch scratch;

    const cv::Mat *effective_mask = &mask;
    if (mask.size() != canvas.size())
    {
        cv::resize(mask, scratch.resized_mask, canvas.size(), 0.0, 0.0, cv::INTER_NEAREST);
        effective_mask = &scratch.resized_mask;
    }

    const cv::Rect active = cv::boundingRect(*effective_mask);
    if (active.empty())
        return true;
    if (alpha >= 1.0)
    {
        canvas(active).setTo(color, (*effective_mask)(active));
        return true;
    }

    const bool color_changed = !scratch.solid_valid || scratch.solid_color != color;
    if (scratch.solid.size() != canvas.size() || scratch.solid.type() != canvas.type())
    {
        scratch.solid.create(canvas.size(), canvas.type());
        scratch.solid_valid = false;
    }
    if (!scratch.solid_valid || color_changed)
    {
        scratch.solid.setTo(color);
        scratch.solid_color = color;
        scratch.solid_valid = true;
    }
    scratch.blended.create(canvas.size(), canvas.type());

    cv::Mat canvas_roi = canvas(active);
    cv::Mat blended_roi = scratch.blended(active);
    cv::addWeighted(canvas_roi, 1.0 - alpha, scratch.solid(active), alpha, 0.0, blended_roi);
    blended_roi.copyTo(canvas_roi, (*effective_mask)(active));
    return true;
}
