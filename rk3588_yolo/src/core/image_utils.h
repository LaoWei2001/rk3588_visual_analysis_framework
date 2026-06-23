#pragma once

#include <opencv2/opencv.hpp>

/**
 * @brief Unified fallback for converting raw frames (NV12/NV21/RGB/BGR) to
 * standard BGR Mat
 * @param pSrcData Raw image data
 * @param srcW Valid width
 * @param srcH Valid height
 * @param srcStrH Horizontal stride
 * @param srcStrV Vertical stride
 * @param srcFmt RK_FORMAT_* enum or equivalent pixel format
 * @param out Output BGR Mat
 * @return true if successful
 */
bool raw_to_bgr_mat(const void *pSrcData, int srcW, int srcH, int srcStrH, int srcStrV, int srcFmt, cv::Mat &out);
