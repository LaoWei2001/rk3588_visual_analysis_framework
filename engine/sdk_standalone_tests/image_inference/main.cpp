// An external-style consumer: only public SDK and OpenCV headers are used.
#include <cstdio>
#include <exception>
#include <stdexcept>
#include <inference.h>
#include <opencv2/imgcodecs.hpp>

int main(int argc, char **argv)
{
    if (argc < 5)
    {
        std::fprintf(stderr, "Usage: %s TYPE MODEL.rknn LABELS.txt|- IMAGE [IMAGE ...]\n", argv[0]);
        return 2;
    }
    try
    {
        ChannelModelConfig detector;
        detector.id = "normal";
        detector.model_type = argv[1];
        detector.model_path = argv[2];
        detector.label_path = std::string(argv[3]) == "-" ? "" : argv[3];
        detector.obj_thresh = .3f;
        detector.nms_thresh = .45f;
        // A second model can also use a different type, file, labels and input size.
        ChannelModelConfig strict = detector;
        strict.id = "strict";
        strict.obj_thresh = .9f;
        vision::ImageInferenceOptions options;
        options.models = {detector, strict};
        vision::ImageInferenceSession session(options); // Load once for the whole batch.
        int failures = 0;
        for (int i = 4; i < argc; ++i)
        {
            try
            {
                cv::Mat bgr = cv::imread(argv[i]);
                const auto result = session.infer(bgr);
                for (const auto &model : result.models)
                    if (!model.succeeded)
                        std::fprintf(stderr, "%s: %s\n", model.model_id.c_str(), model.error.c_str());
                for (const auto &target : result.results)
                    std::printf("%s %s %.2f\n", target.model_id.c_str(), target.label.c_str(), target.score);
                vision::draw_inference_results(bgr, result);
                if (!cv::imwrite("result-" + std::to_string(i - 4) + ".jpg", bgr))
                    throw std::runtime_error("Cannot save annotated image");
                if (!result.complete())
                    ++failures;
            }
            catch (const std::exception &error)
            {
                ++failures;
                std::fprintf(stderr, "%s: %s\n", argv[i], error.what());
            }
        }
        return failures ? 1 : 0;
    }
    catch (const std::exception &error)
    {
        std::fprintf(stderr, "%s\n", error.what());
        return 2;
    }
}
