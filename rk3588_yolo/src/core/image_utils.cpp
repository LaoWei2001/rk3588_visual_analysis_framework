#include "image_utils.h"
#include "../system.h"
#include <rga/RgaApi.h>

bool raw_to_bgr_mat(const void *pSrcData, int srcW, int srcH, int srcStrH, int srcStrV, int srcFmt, cv::Mat &out)
{
    out.release();
    if (!pSrcData || srcW <= 0 || srcH <= 0 || srcStrH <= 0 || srcStrV <= 0) {
        log_printf_threadsafe("[ImageUtils] Invalid input arguments! pSrcData=%p, w=%d, h=%d, strW=%d, strH=%d\n",
                              pSrcData, srcW, srcH, srcStrH, srcStrV);
        return false;
    }

    // NV12 = 0x1, NV21 = 0x2, BGR = 0x3, RGB = 0x4 (RK_FORMAT_*) 
    // Wait, the caller passes either RK_FORMAT_* or pixel_format from channel_raw.
    // Let's rely on RK_FORMAT_ constants from RgaApi.h
    // RK_FORMAT_YCbCr_420_SP = 0x0A, RK_FORMAT_YCrCb_420_SP = 0x0B
    // RK_FORMAT_BGR_888 = 0x0D, RK_FORMAT_RGB_888 = 0x0E

    // Extract base format using mask, as RK_FORMAT_ constants might have context/version high bits
    int baseFmt = srcFmt & 0x0FFF;

    if (baseFmt == 0x0D /* RK_FORMAT_BGR_888 */) {
        out = cv::Mat(srcH, srcW, CV_8UC3, const_cast<void*>(pSrcData)).clone();
        return true;
    }
    if (baseFmt == 0x0E /* RK_FORMAT_RGB_888 */) {
        cv::Mat rgb(srcH, srcW, CV_8UC3, const_cast<void*>(pSrcData));
        cv::cvtColor(rgb, out, cv::COLOR_RGB2BGR);
        return true;
    }

    // NV12/NV21 (0x0A / 0x0B, or 0xA00 / 0xB00 if shifted)
    // Wait, the log showed 0xA00 exactly. Let's handle both possible shifts.
    if (baseFmt == 0x0A || baseFmt == 0xA00 || srcFmt == 0xA00 || 
        baseFmt == 0x0B || baseFmt == 0xB00 || srcFmt == 0xB00) {
        int code = (baseFmt == 0x0A || baseFmt == 0xA00 || srcFmt == 0xA00) ? cv::COLOR_YUV2BGR_NV12 : cv::COLOR_YUV2BGR_NV21;
        try {
            cv::Mat yuv(srcStrV * 3 / 2, srcStrH, CV_8UC1, const_cast<void*>(pSrcData));
            cv::cvtColor(yuv, out, code);
            if (!out.empty() && (out.cols > srcW || out.rows > srcH)) {
                out = out(cv::Rect(0, 0, srcW, srcH)).clone(); // Keep continuous
            }
            return true;
        } catch (const cv::Exception& e) {
            log_printf_threadsafe("[ImageUtils] cvtColor exception: %s\n", e.what());
            return false;
        } catch (...) {
            log_printf_threadsafe("[ImageUtils] cvtColor unknown exception caught!\n");
            return false;
        }
    }
    
    log_printf_threadsafe("[ImageUtils] Unsupported srcFmt: 0x%02X (%d)\n", srcFmt, srcFmt);
    return false;
}
