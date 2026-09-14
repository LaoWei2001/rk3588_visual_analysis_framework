#ifndef __RKNN_YOLO11_H__
#define __RKNN_YOLO11_H__

#include "opencv2/opencv.hpp"
#include "rknn_api.h"

#define YOLO11_OBJ_CLASS_NUM		80

/**< @brief yolo11模型结构体 */
typedef struct {
	rknn_context rknn_ctx;
	rknn_input_output_num io_num;
	rknn_tensor_attr *input_attrs;
	rknn_tensor_attr *output_attrs;
	int model_channel;
	int model_width;
	int model_height;
	bool is_quant;
} rknn_yolo11_context_t;


/**< @brief yolo11检测结果 */
typedef struct {
	int		cls_id;
	int		left;
	int		top;
	int		right;
	int		bottom;
	float	prop;
} rknn_yolo11_result_t;


#ifdef __cplusplus
extern "C" {
#endif
	/**
	* @brief  rknn yolo11初始化函数
	*
	* @param[in]		p_model_path			yolo11 rknn模型地址
	* @param[i/o]		p_yolo11				yolo11模型上下文
	* @return									模型初始化结果
	*/
	int yolov11_detect_init(const char *p_model_path, rknn_yolo11_context_t *p_yolo11);


	/**
	* @brief  rknn yolo11计算函数
	*
	* @param[in]		image					待检测图片
	* @param[in]		p_yolo11				yolo11模型上下文
	* @param[in]		nms_threshold			NMS阈值
	* @param[in]		conf_threshold			置信度阈值
	* @return									检测结果
	*/
	std::vector<rknn_yolo11_result_t> yolov11_detect_run(cv::Mat image, rknn_yolo11_context_t *p_yolo11, float nms_threshold, float conf_threshold);


	/**
	* @brief  rknn yolo11释放函数
	*
	* @param[i/o]		p_yolo11				yolo11模型上下文
	* @return									模型释放结果
	*/
	int yolov11_detect_release(rknn_yolo11_context_t *p_yolo11);


#ifdef __cplusplus
}
#endif

#endif // __RKNN_YOLO11_H__
