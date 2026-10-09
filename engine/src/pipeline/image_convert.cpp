#include "image_convert.h"
#include "common/logging.h"
#include <rga/RgaApi.h>

bool convert_raw_to_bgr(const void *pSrcData, int srcW, int srcH, int srcStrH, int srcStrV, int srcFmt, cv::Mat &out)
{
    if (!pSrcData || srcW <= 0 || srcH <= 0 || srcStrH < srcW || srcStrV < srcH)
    {
        out.release();
        log_printf_threadsafe("[ImageUtils] Invalid input arguments! pSrcData=%p, w=%d, h=%d, strW=%d, strH=%d\n",
                              pSrcData, srcW, srcH, srcStrH, srcStrV);
        return false;
    }

    // 使用当前 RGA SDK 的真实枚举；保留历史软件帧 0x0D/0x0E 编码。
    const int baseFmt = srcFmt & 0x0FFF;

    if (srcFmt == RK_FORMAT_BGR_888 || baseFmt == 0x0D)
    {
        cv::Mat bgr_padded(srcStrV, srcStrH, CV_8UC3, const_cast<void *>(pSrcData), static_cast<size_t>(srcStrH) * 3);
        bgr_padded(cv::Rect(0, 0, srcW, srcH)).copyTo(out); // 复用 worker 输出内存。
        return true;
    }
    if (srcFmt == RK_FORMAT_RGB_888 || baseFmt == 0x0E)
    {
        cv::Mat rgb_padded(srcStrV, srcStrH, CV_8UC3, const_cast<void *>(pSrcData), static_cast<size_t>(srcStrH) * 3);
        cv::cvtColor(rgb_padded(cv::Rect(0, 0, srcW, srcH)), out, cv::COLOR_RGB2BGR);
        return true;
    }

    // NV12/NV21 (0x0A / 0x0B, or 0xA00 / 0xB00 if shifted)
    if (baseFmt == 0x0A || baseFmt == 0xA00 || srcFmt == 0xA00 || baseFmt == 0x0B || baseFmt == 0xB00 ||
        srcFmt == 0xB00)
    {
        int code =
            (baseFmt == 0x0A || baseFmt == 0xA00 || srcFmt == 0xA00) ? cv::COLOR_YUV2BGR_NV12 : cv::COLOR_YUV2BGR_NV21;
        try
        {
            cv::Mat yuv(srcStrV * 3 / 2, srcStrH, CV_8UC1, const_cast<void *>(pSrcData));
            cv::cvtColor(yuv, out, code);
            if (!out.empty() && (out.cols > srcW || out.rows > srcH))
            {
                out = out(cv::Rect(0, 0, srcW, srcH)).clone(); // Keep continuous
            }
            return true;
        }
        catch (const cv::Exception &e)
        {
            out.release();
            log_printf_threadsafe("[ImageUtils] cvtColor exception: %s\n", e.what());
            return false;
        }
        catch (...)
        {
            out.release();
            log_printf_threadsafe("[ImageUtils] cvtColor unknown exception caught!\n");
            return false;
        }
    }

    out.release();
    log_printf_threadsafe("[ImageUtils] Unsupported srcFmt: 0x%02X (%d)\n", srcFmt, srcFmt);
    return false;
}
