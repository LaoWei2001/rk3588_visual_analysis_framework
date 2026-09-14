#include "yolo11.h"

/**< @brief letterbox数据结构体 */
typedef struct {
	int x_pad;
	int y_pad;
	float scale;
} __yolo11_letterbox_t;


int yolo11_read_data_from_file(const char *path, char **out_data);
static void yolo11_dump_tensor_attr(rknn_tensor_attr *attr);
cv::Mat yolo11_letter_box(const cv::Mat src, int target_width, int target_height, __yolo11_letterbox_t &letter_box);
std::vector<rknn_yolo11_result_t> yolo11_post_process(rknn_yolo11_context_t *p_yolo11, void *outputs, __yolo11_letterbox_t letter_box, float conf_threshold, float nms_threshold);

/*******************************************
* yolov11_detect_init
********************************************/
int yolov11_detect_init(const char *p_model_path, rknn_yolo11_context_t *p_yolo11)
{
	int ret;
	int model_len = 0;
	char *model;
	rknn_context ctx = 0;

	// Load RKNN Model
	model_len = yolo11_read_data_from_file(p_model_path, &model);
	if (model == NULL){
		printf("load_model fail!\n");
		return -1;
	}
	ret = rknn_init(&ctx, model, model_len, 0, NULL);
	free(model);
	if (ret < 0){
		printf("rknn_init fail! ret=%d\n", ret);
		return -1;
	}

	// Get Model Input Output Number
	rknn_input_output_num io_num;
	ret = rknn_query(ctx, RKNN_QUERY_IN_OUT_NUM, &io_num, sizeof(io_num));
	if (ret != RKNN_SUCC){
		printf("rknn_query fail! ret=%d\n", ret);
		return -1;
	}
	printf("model input num: %d, output num: %d\n", io_num.n_input, io_num.n_output);

	// Get Model Input Info
	printf("input tensors:\n");
	rknn_tensor_attr input_attrs[io_num.n_input];
	memset(input_attrs, 0, sizeof(input_attrs));
	for (int i = 0; i < io_num.n_input; i++){
		input_attrs[i].index = i;
		ret = rknn_query(ctx, RKNN_QUERY_INPUT_ATTR, &(input_attrs[i]), sizeof(rknn_tensor_attr));
		if (ret != RKNN_SUCC){
			printf("rknn_query fail! ret=%d\n", ret);
			return -1;
		}
		yolo11_dump_tensor_attr(&(input_attrs[i]));
	}

	// Get Model Output Info
	printf("output tensors:\n");
	rknn_tensor_attr output_attrs[io_num.n_output];
	memset(output_attrs, 0, sizeof(output_attrs));
	for (int i = 0; i < io_num.n_output; i++){
		output_attrs[i].index = i;
		ret = rknn_query(ctx, RKNN_QUERY_OUTPUT_ATTR, &(output_attrs[i]), sizeof(rknn_tensor_attr));
		if (ret != RKNN_SUCC)
		{
			printf("rknn_query fail! ret=%d\n", ret);
			return -1;
		}
		yolo11_dump_tensor_attr(&(output_attrs[i]));
	}

	// Set to context
	p_yolo11->rknn_ctx = ctx;

	// TODO
	if (output_attrs[0].qnt_type == RKNN_TENSOR_QNT_AFFINE_ASYMMETRIC && output_attrs[0].type == RKNN_TENSOR_INT8){
		p_yolo11->is_quant = true;
	}
	else{
		p_yolo11->is_quant = false;
	}

	p_yolo11->io_num = io_num;
	p_yolo11->input_attrs = (rknn_tensor_attr *)malloc(io_num.n_input * sizeof(rknn_tensor_attr));
	memcpy(p_yolo11->input_attrs, input_attrs, io_num.n_input * sizeof(rknn_tensor_attr));
	p_yolo11->output_attrs = (rknn_tensor_attr *)malloc(io_num.n_output * sizeof(rknn_tensor_attr));
	memcpy(p_yolo11->output_attrs, output_attrs, io_num.n_output * sizeof(rknn_tensor_attr));

	if (input_attrs[0].fmt == RKNN_TENSOR_NCHW)
	{
		printf("model is NCHW input fmt\n");
		p_yolo11->model_channel = input_attrs[0].dims[1];
		p_yolo11->model_height = input_attrs[0].dims[2];
		p_yolo11->model_width = input_attrs[0].dims[3];
	}
	else{
		printf("model is NHWC input fmt\n");
		p_yolo11->model_height = input_attrs[0].dims[1];
		p_yolo11->model_width = input_attrs[0].dims[2];
		p_yolo11->model_channel = input_attrs[0].dims[3];
	}
	printf("model input height=%d, width=%d, channel=%d\n", 
		p_yolo11->model_height, p_yolo11->model_width, p_yolo11->model_channel);

	return 0;
}


/*******************************************
* yolov11_detect_run
********************************************/
std::vector<rknn_yolo11_result_t> yolov11_detect_run(cv::Mat image, rknn_yolo11_context_t *p_yolo11, float nms_threshold, float conf_threshold)
{
	std::vector<rknn_yolo11_result_t> results;

	rknn_input inputs[p_yolo11->io_num.n_input];
	rknn_output outputs[p_yolo11->io_num.n_output];
	if ((!p_yolo11) || image.empty()) {
		return results;
	}

	// 预处理
	__yolo11_letterbox_t letter_box;
	cv::Mat pre_image = yolo11_letter_box(image, p_yolo11->model_width, p_yolo11->model_height, letter_box);

	// Set Input Data
	inputs[0].index = 0;
	inputs[0].type = RKNN_TENSOR_UINT8;
	inputs[0].fmt = RKNN_TENSOR_NHWC;
	inputs[0].size = p_yolo11->model_width * p_yolo11->model_height * p_yolo11->model_channel;
	inputs[0].buf = pre_image.data;

	int ret = rknn_inputs_set(p_yolo11->rknn_ctx, p_yolo11->io_num.n_input, inputs);
	if (ret < 0){
		printf("rknn_input_set fail! ret=%d\n", ret);
		return results;
	}

	// Run
	ret = rknn_run(p_yolo11->rknn_ctx, nullptr);
	if (ret < 0){
		printf("rknn_run fail! ret=%d\n", ret);
		return results;
	}

	// Get Output
	memset(outputs, 0, sizeof(outputs));
	for (int i = 0; i < p_yolo11->io_num.n_output; i++) {
		outputs[i].index = i;
		outputs[i].want_float = (!p_yolo11->is_quant);
	}
	ret = rknn_outputs_get(p_yolo11->rknn_ctx, p_yolo11->io_num.n_output, outputs, NULL);
	if (ret < 0) {
		printf("rknn_outputs_get fail! ret=%d\n", ret);
		goto out;
	}

	// Post Process
	results = yolo11_post_process(p_yolo11, outputs, letter_box, conf_threshold, nms_threshold);

	// Remeber to release rknn output
	rknn_outputs_release(p_yolo11->rknn_ctx, p_yolo11->io_num.n_output, outputs);

out:
	pre_image.release();

	return results;
}


/*******************************************
* yolov11_detect_release
********************************************/
int yolov11_detect_release(rknn_yolo11_context_t *p_yolo11)
{
	if (p_yolo11->input_attrs != NULL)
	{
		free(p_yolo11->input_attrs);
		p_yolo11->input_attrs = NULL;
	}
	if (p_yolo11->output_attrs != NULL)
	{
		free(p_yolo11->output_attrs);
		p_yolo11->output_attrs = NULL;
	}
	if (p_yolo11->rknn_ctx != 0)
	{
		rknn_destroy(p_yolo11->rknn_ctx);
		p_yolo11->rknn_ctx = 0;
	}
	return 0;
}



/*---------------------------------------------* private *-----------------------------------------------*/

/// 读取模型数据
int yolo11_read_data_from_file(const char *path, char **out_data)
{
	FILE *fp = fopen(path, "rb");
	if (fp == NULL) {
		printf("fopen %s fail!\n", path);
		return -1;
	}
	fseek(fp, 0, SEEK_END);
	int file_size = ftell(fp);
	char *data = (char *)malloc(file_size + 1);
	data[file_size] = 0;
	fseek(fp, 0, SEEK_SET);
	if (file_size != fread(data, 1, file_size, fp)) {
		printf("fread %s fail!\n", path);
		free(data);
		fclose(fp);
		return -1;
	}
	if (fp) {
		fclose(fp);
	}
	*out_data = data;
	return file_size;
}


/// 输出tensor信息
static void yolo11_dump_tensor_attr(rknn_tensor_attr *attr)
{
	printf("  index=%d, name=%s, n_dims=%d, dims=[%d, %d, %d, %d], n_elems=%d, size=%d, fmt=%s, type=%s, qnt_type=%s, "
		"zp=%d, scale=%f\n",
		attr->index, attr->name, attr->n_dims, attr->dims[0], attr->dims[1], attr->dims[2], attr->dims[3],
		attr->n_elems, attr->size, get_format_string(attr->fmt), get_type_string(attr->type),
		get_qnt_type_string(attr->qnt_type), attr->zp, attr->scale);
}


/// Letter box函数
cv::Mat yolo11_letter_box(const cv::Mat src, int target_width, int target_height, __yolo11_letterbox_t &letter_box) 
{
	letter_box.scale = 1.0f;
	letter_box.x_pad = 0;
	letter_box.y_pad = 0;
	if (src.rows == target_height && src.cols == target_width) {
		return src;
	}

	// 获取原始图像的宽高
	int src_width = src.cols;
	int src_height = src.rows;

	// 计算缩放比例
	letter_box.scale = std::min(static_cast<float>(target_width) / src_width, static_cast<float>(target_height) / src_height);

	// 计算缩放后的图像尺寸
	int new_width = static_cast<int>(src_width * letter_box.scale);
	int new_height = static_cast<int>(src_height * letter_box.scale);

	// 计算填充的边界
	letter_box.x_pad= (target_width - new_width) / 2;
	letter_box.y_pad = (target_height - new_height) / 2;

	// 缩放图像
	cv::Mat resized;
	cv::resize(src, resized, cv::Size(new_width, new_height), 0, 0, cv::INTER_LINEAR);

	// 创建目标图像并填充边界
	cv::Mat dst = cv::Mat::zeros(target_height, target_width, src.type());
	resized.copyTo(dst(cv::Rect(letter_box.x_pad, letter_box.y_pad, new_width, new_height)));

	resized.release();
	return dst;
}


/// yolo11 后处理
inline static int clamp(float val, int min, int max) { return val > min ? (val < max ? val : max) : min; }

static float CalculateOverlap(float xmin0, float ymin0, float xmax0, float ymax0, float xmin1, float ymin1, float xmax1,
	float ymax1)
{
	float w = fmax(0.f, fmin(xmax0, xmax1) - fmax(xmin0, xmin1) + 1.0);
	float h = fmax(0.f, fmin(ymax0, ymax1) - fmax(ymin0, ymin1) + 1.0);
	float i = w * h;
	float u = (xmax0 - xmin0 + 1.0) * (ymax0 - ymin0 + 1.0) + (xmax1 - xmin1 + 1.0) * (ymax1 - ymin1 + 1.0) - i;
	return u <= 0.f ? 0.f : (i / u);
}

static int nms(int validCount, std::vector<float> &outputLocations, std::vector<int> classIds, std::vector<int> &order,
	int filterId, float threshold)
{
	for (int i = 0; i < validCount; ++i)
	{
		int n = order[i];
		if (n == -1 || classIds[n] != filterId)
		{
			continue;
		}
		for (int j = i + 1; j < validCount; ++j)
		{
			int m = order[j];
			if (m == -1 || classIds[m] != filterId)
			{
				continue;
			}
			float xmin0 = outputLocations[n * 4 + 0];
			float ymin0 = outputLocations[n * 4 + 1];
			float xmax0 = outputLocations[n * 4 + 0] + outputLocations[n * 4 + 2];
			float ymax0 = outputLocations[n * 4 + 1] + outputLocations[n * 4 + 3];

			float xmin1 = outputLocations[m * 4 + 0];
			float ymin1 = outputLocations[m * 4 + 1];
			float xmax1 = outputLocations[m * 4 + 0] + outputLocations[m * 4 + 2];
			float ymax1 = outputLocations[m * 4 + 1] + outputLocations[m * 4 + 3];

			float iou = CalculateOverlap(xmin0, ymin0, xmax0, ymax0, xmin1, ymin1, xmax1, ymax1);

			if (iou > threshold)
			{
				order[j] = -1;
			}
		}
	}
	return 0;
}

static int quick_sort_indice_inverse(std::vector<float> &input, int left, int right, std::vector<int> &indices)
{
	float key;
	int key_index;
	int low = left;
	int high = right;
	if (left < right)
	{
		key_index = indices[left];
		key = input[left];
		while (low < high)
		{
			while (low < high && input[high] <= key)
			{
				high--;
			}
			input[low] = input[high];
			indices[low] = indices[high];
			while (low < high && input[low] >= key)
			{
				low++;
			}
			input[high] = input[low];
			indices[high] = indices[low];
		}
		input[low] = key;
		indices[low] = key_index;
		quick_sort_indice_inverse(input, left, low - 1, indices);
		quick_sort_indice_inverse(input, low + 1, right, indices);
	}
	return low;
}

static float sigmoid(float x) { return 1.0 / (1.0 + expf(-x)); }

static float unsigmoid(float y) { return -1.0 * logf((1.0 / y) - 1.0); }

inline static int32_t __clip(float val, float min, float max)
{
	float f = val <= min ? min : (val >= max ? max : val);
	return f;
}

static int8_t qnt_f32_to_affine(float f32, int32_t zp, float scale)
{
	float dst_val = (f32 / scale) + zp;
	int8_t res = (int8_t)__clip(dst_val, -128, 127);
	return res;
}

static uint8_t qnt_f32_to_affine_u8(float f32, int32_t zp, float scale)
{
	float dst_val = (f32 / scale) + zp;
	uint8_t res = (uint8_t)__clip(dst_val, 0, 255);
	return res;
}

static float deqnt_affine_to_f32(int8_t qnt, int32_t zp, float scale) { return ((float)qnt - (float)zp) * scale; }

static float deqnt_affine_u8_to_f32(uint8_t qnt, int32_t zp, float scale) { return ((float)qnt - (float)zp) * scale; }

static void compute_dfl(float* tensor, int dfl_len, float* box) {
	for (int b = 0; b<4; b++) {
		float exp_t[dfl_len];
		float exp_sum = 0;
		float acc_sum = 0;
		for (int i = 0; i< dfl_len; i++) {
			exp_t[i] = exp(tensor[i + b*dfl_len]);
			exp_sum += exp_t[i];
		}

		for (int i = 0; i< dfl_len; i++) {
			acc_sum += exp_t[i] / exp_sum *i;
		}
		box[b] = acc_sum;
	}
}

static int process_u8(uint8_t *box_tensor, int32_t box_zp, float box_scale,
	uint8_t *score_tensor, int32_t score_zp, float score_scale,
	uint8_t *score_sum_tensor, int32_t score_sum_zp, float score_sum_scale,
	int grid_h, int grid_w, int stride, int dfl_len,
	std::vector<float> &boxes,
	std::vector<float> &objProbs,
	std::vector<int> &classId,
	float threshold)
{
	int validCount = 0;
	int grid_len = grid_h * grid_w;
	uint8_t score_thres_u8 = qnt_f32_to_affine_u8(threshold, score_zp, score_scale);
	uint8_t score_sum_thres_u8 = qnt_f32_to_affine_u8(threshold, score_sum_zp, score_sum_scale);

	for (int i = 0; i < grid_h; i++)
	{
		for (int j = 0; j < grid_w; j++)
		{
			int offset = i * grid_w + j;
			int max_class_id = -1;

			// Use score sum to quickly filter
			if (score_sum_tensor != nullptr)
			{
				if (score_sum_tensor[offset] < score_sum_thres_u8)
				{
					continue;
				}
			}

			uint8_t max_score = -score_zp;
			for (int c = 0; c < YOLO11_OBJ_CLASS_NUM; c++)
			{
				if ((score_tensor[offset] > score_thres_u8) && (score_tensor[offset] > max_score))
				{
					max_score = score_tensor[offset];
					max_class_id = c;
				}
				offset += grid_len;
			}

			// compute box
			if (max_score > score_thres_u8)
			{
				offset = i * grid_w + j;
				float box[4];
				float before_dfl[dfl_len * 4];
				for (int k = 0; k < dfl_len * 4; k++)
				{
					before_dfl[k] = deqnt_affine_u8_to_f32(box_tensor[offset], box_zp, box_scale);
					offset += grid_len;
				}
				compute_dfl(before_dfl, dfl_len, box);

				float x1, y1, x2, y2, w, h;
				x1 = (-box[0] + j + 0.5) * stride;
				y1 = (-box[1] + i + 0.5) * stride;
				x2 = (box[2] + j + 0.5) * stride;
				y2 = (box[3] + i + 0.5) * stride;
				w = x2 - x1;
				h = y2 - y1;
				boxes.push_back(x1);
				boxes.push_back(y1);
				boxes.push_back(w);
				boxes.push_back(h);

				objProbs.push_back(deqnt_affine_u8_to_f32(max_score, score_zp, score_scale));
				classId.push_back(max_class_id);
				validCount++;
			}
		}
	}
	return validCount;
}

static int process_i8(int8_t *box_tensor, int32_t box_zp, float box_scale,
	int8_t *score_tensor, int32_t score_zp, float score_scale,
	int8_t *score_sum_tensor, int32_t score_sum_zp, float score_sum_scale,
	int grid_h, int grid_w, int stride, int dfl_len,
	std::vector<float> &boxes,
	std::vector<float> &objProbs,
	std::vector<int> &classId,
	float threshold)
{
	int validCount = 0;
	int grid_len = grid_h * grid_w;
	int8_t score_thres_i8 = qnt_f32_to_affine(threshold, score_zp, score_scale);
	int8_t score_sum_thres_i8 = qnt_f32_to_affine(threshold, score_sum_zp, score_sum_scale);

	for (int i = 0; i < grid_h; i++)
	{
		for (int j = 0; j < grid_w; j++)
		{
			int offset = i* grid_w + j;
			int max_class_id = -1;

			// 通过 score sum 起到快速过滤的作用
			if (score_sum_tensor != nullptr) {
				if (score_sum_tensor[offset] < score_sum_thres_i8) {
					continue;
				}
			}

			int8_t max_score = -score_zp;
			for (int c = 0; c< YOLO11_OBJ_CLASS_NUM; c++) {
				if ((score_tensor[offset] > score_thres_i8) && (score_tensor[offset] > max_score))
				{
					max_score = score_tensor[offset];
					max_class_id = c;
				}
				offset += grid_len;
			}

			// compute box
			if (max_score> score_thres_i8) {
				offset = i* grid_w + j;
				float box[4];
				float before_dfl[dfl_len * 4];
				for (int k = 0; k< dfl_len * 4; k++) {
					before_dfl[k] = deqnt_affine_to_f32(box_tensor[offset], box_zp, box_scale);
					offset += grid_len;
				}
				compute_dfl(before_dfl, dfl_len, box);

				float x1, y1, x2, y2, w, h;
				x1 = (-box[0] + j + 0.5)*stride;
				y1 = (-box[1] + i + 0.5)*stride;
				x2 = (box[2] + j + 0.5)*stride;
				y2 = (box[3] + i + 0.5)*stride;
				w = x2 - x1;
				h = y2 - y1;
				boxes.push_back(x1);
				boxes.push_back(y1);
				boxes.push_back(w);
				boxes.push_back(h);

				objProbs.push_back(deqnt_affine_to_f32(max_score, score_zp, score_scale));
				classId.push_back(max_class_id);
				validCount++;
			}
		}
	}
	return validCount;
}

static int process_fp32(float *box_tensor, float *score_tensor, float *score_sum_tensor,
	int grid_h, int grid_w, int stride, int dfl_len,
	std::vector<float> &boxes,
	std::vector<float> &objProbs,
	std::vector<int> &classId,
	float threshold)
{
	int validCount = 0;
	int grid_len = grid_h * grid_w;
	for (int i = 0; i < grid_h; i++)
	{
		for (int j = 0; j < grid_w; j++)
		{
			int offset = i* grid_w + j;
			int max_class_id = -1;

			// 通过 score sum 起到快速过滤的作用
			if (score_sum_tensor != nullptr) {
				if (score_sum_tensor[offset] < threshold) {
					continue;
				}
			}

			float max_score = 0;
			for (int c = 0; c< YOLO11_OBJ_CLASS_NUM; c++) {
				if ((score_tensor[offset] > threshold) && (score_tensor[offset] > max_score))
				{
					max_score = score_tensor[offset];
					max_class_id = c;
				}
				offset += grid_len;
			}

			// compute box
			if (max_score> threshold) {
				offset = i* grid_w + j;
				float box[4];
				float before_dfl[dfl_len * 4];
				for (int k = 0; k< dfl_len * 4; k++) {
					before_dfl[k] = box_tensor[offset];
					offset += grid_len;
				}
				compute_dfl(before_dfl, dfl_len, box);

				float x1, y1, x2, y2, w, h;
				x1 = (-box[0] + j + 0.5)*stride;
				y1 = (-box[1] + i + 0.5)*stride;
				x2 = (box[2] + j + 0.5)*stride;
				y2 = (box[3] + i + 0.5)*stride;
				w = x2 - x1;
				h = y2 - y1;
				boxes.push_back(x1);
				boxes.push_back(y1);
				boxes.push_back(w);
				boxes.push_back(h);

				objProbs.push_back(max_score);
				classId.push_back(max_class_id);
				validCount++;
			}
		}
	}
	return validCount;
}

std::vector<rknn_yolo11_result_t> yolo11_post_process(rknn_yolo11_context_t *p_yolo11, void *outputs, __yolo11_letterbox_t letter_box, float conf_threshold, float nms_threshold)
{
	std::vector<rknn_yolo11_result_t> results;
	rknn_output *_outputs = (rknn_output *)outputs;
	std::vector<float> filterBoxes;
	std::vector<float> objProbs;
	std::vector<int> classId;
	int validCount = 0;
	int stride = 0;
	int grid_h = 0;
	int grid_w = 0;
	int model_in_w = p_yolo11->model_width;
	int model_in_h = p_yolo11->model_height;

	// default 3 branch
#ifdef RKNPU1
	int dfl_len = p_yolo11->output_attrs[0].dims[2] / 4;
#else
	int dfl_len = p_yolo11->output_attrs[0].dims[1] / 4;
#endif
	int output_per_branch = p_yolo11->io_num.n_output / 3;
	for (int i = 0; i < 3; i++)
	{
		void *score_sum = nullptr;
		int32_t score_sum_zp = 0;
		float score_sum_scale = 1.0;
		if (output_per_branch == 3) {
			score_sum = _outputs[i*output_per_branch + 2].buf;
			score_sum_zp = p_yolo11->output_attrs[i*output_per_branch + 2].zp;
			score_sum_scale = p_yolo11->output_attrs[i*output_per_branch + 2].scale;
		}
		int box_idx = i*output_per_branch;
		int score_idx = i*output_per_branch + 1;

#ifdef RKNPU1
		grid_h = p_yolo11->output_attrs[box_idx].dims[1];
		grid_w = p_yolo11->output_attrs[box_idx].dims[0];
#else
		grid_h = p_yolo11->output_attrs[box_idx].dims[2];
		grid_w = p_yolo11->output_attrs[box_idx].dims[3];
#endif
		stride = model_in_h / grid_h;

		if (p_yolo11->is_quant)
		{
#ifdef RKNPU1
			validCount += process_u8((uint8_t *)_outputs[box_idx].buf, p_yolo11->output_attrs[box_idx].zp, p_yolo11->output_attrs[box_idx].scale,
				(uint8_t *)_outputs[score_idx].buf, p_yolo11->output_attrs[score_idx].zp, p_yolo11->output_attrs[score_idx].scale,
				(uint8_t *)score_sum, score_sum_zp, score_sum_scale,
				grid_h, grid_w, stride, dfl_len,
				filterBoxes, objProbs, classId, conf_threshold);
#else
			validCount += process_i8((int8_t *)_outputs[box_idx].buf, p_yolo11->output_attrs[box_idx].zp, p_yolo11->output_attrs[box_idx].scale,
				(int8_t *)_outputs[score_idx].buf, p_yolo11->output_attrs[score_idx].zp, p_yolo11->output_attrs[score_idx].scale,
				(int8_t *)score_sum, score_sum_zp, score_sum_scale,
				grid_h, grid_w, stride, dfl_len,
				filterBoxes, objProbs, classId, conf_threshold);
#endif
		}
		else
		{
			validCount += process_fp32((float *)_outputs[box_idx].buf, (float *)_outputs[score_idx].buf, (float *)score_sum,
				grid_h, grid_w, stride, dfl_len,
				filterBoxes, objProbs, classId, conf_threshold);
		}
	}


	// no object detect
	if (validCount <= 0){
		return results;
	}
	std::vector<int> indexArray;
	for (int i = 0; i < validCount; ++i)
	{
		indexArray.push_back(i);
	}
	quick_sort_indice_inverse(objProbs, 0, validCount - 1, indexArray);

	std::set<int> class_set(std::begin(classId), std::end(classId));

	for (auto c : class_set)
	{
		nms(validCount, filterBoxes, classId, indexArray, c, nms_threshold);
	}

	for (int i = 0; i < validCount; ++i){
		if (indexArray[i] == -1){
			continue;
		}
		int n = indexArray[i];

		float x1 = filterBoxes[n * 4 + 0] - letter_box.x_pad;
		float y1 = filterBoxes[n * 4 + 1] - letter_box.y_pad;
		float x2 = x1 + filterBoxes[n * 4 + 2];
		float y2 = y1 + filterBoxes[n * 4 + 3];
		int id = classId[n];
		float obj_conf = objProbs[i];

		rknn_yolo11_result_t tmp;
		tmp.left = (int)(clamp(x1, 0, model_in_w) / letter_box.scale);
		tmp.top = (int)(clamp(y1, 0, model_in_h) / letter_box.scale);
		tmp.right = (int)(clamp(x2, 0, model_in_w) / letter_box.scale);
		tmp.bottom = (int)(clamp(y2, 0, model_in_h) / letter_box.scale);
		tmp.prop = obj_conf;
		tmp.cls_id = id;
		results.push_back(tmp);
	}
	
	return results;
}
