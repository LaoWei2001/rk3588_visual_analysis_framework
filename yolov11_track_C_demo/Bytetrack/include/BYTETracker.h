#pragma once

#include "STrack.h"

struct Object
{
    int classId;
    float score;
    cv::Rect_<float> box;
};

class BYTETracker
{
public:

	/**
	* @brief  bytetracker 构造函数
	*
	* @param[in]		track_thresh			跟踪阈值：小于该阈值的为低分检测框，大于该值的为高分检测框
	* @param[in]		high_thresh				高阈值：检测框阈值大于该阈值可新启航迹，high_thresh一般大于track_thresh
	* @param[in]		match_thresh			匹配阈值：目标前后帧匹配相似度，值越大越相似；大于阈值则判断前后帧目标匹配（跟踪）成功；
	* @param[in]		frame_rate				使用帧率，用于计算目标最大丢失时长：max_time_lost = int(frame_rate / 30.0 * track_buffer);
	* @param[in]		track_buffer			跟踪空间大小，用于计算目标最大丢失时长：max_time_lost = int(frame_rate / 30.0 * track_buffer);
	* TODO： 
	*	Tracker匹配策略，过程共两次匹配：
	*	第一次：所有航迹与高分检测框匹配，若匹配失败的高分检测框且大于high_thresh时，新启航迹。
	*	第二次：匹配失败的航迹与低分检测框再匹配，匹配失败的低分检测框记为背景，匹配失败的航迹记为lost等待重生或消亡（连续max_time_lost帧都失败）
	*/
	BYTETracker(float track_thresh, float high_thresh, float match_thresh, int frame_rate = 30, int track_buffer = 30);
	
	/*
	* @brief   bytetracker 跟踪函数（跟新跟踪对象）
	*
	* @param[in]	objects		当前检测对象信息
	* @return					返回跟踪结果信息
	*/
	std::vector<STrack> update(const  std::vector<Object>& objects);

	/// @brief 根据idx生成颜色
    cv::Scalar get_color(int idx);

	/// @brief bytetracker析构函数
	~BYTETracker();

private:
	 std::vector<STrack*> joint_stracks( std::vector<STrack*> &tlista,  std::vector<STrack> &tlistb);
	 std::vector<STrack> joint_stracks( std::vector<STrack> &tlista,  std::vector<STrack> &tlistb);

	 std::vector<STrack> sub_stracks( std::vector<STrack> &tlista,  std::vector<STrack> &tlistb);
	void remove_duplicate_stracks( std::vector<STrack> &resa,  std::vector<STrack> &resb,  std::vector<STrack> &stracksa,  std::vector<STrack> &stracksb);

	void linear_assignment( std::vector< std::vector<float> > &cost_matrix, int cost_matrix_size, int cost_matrix_size_size, float thresh,
		 std::vector< std::vector<int> > &matches,  std::vector<int> &unmatched_a,  std::vector<int> &unmatched_b);
	 std::vector< std::vector<float> > iou_distance( std::vector<STrack*> &atracks,  std::vector<STrack> &btracks, int &dist_size, int &dist_size_size);
	 std::vector< std::vector<float> > iou_distance( std::vector<STrack> &atracks,  std::vector<STrack> &btracks);
	 std::vector< std::vector<float> > ious( std::vector< std::vector<float> > &atlbrs,  std::vector< std::vector<float> > &btlbrs);

	double lapjv(const  std::vector< std::vector<float> > &cost,  std::vector<int> &rowsol,  std::vector<int> &colsol, 
		bool extend_cost = false, float cost_limit = LONG_MAX, bool return_cost = true);

private:

	float track_thresh;
	float high_thresh;
	float match_thresh;
	int frame_id;
	int max_time_lost;

	 std::vector<STrack> tracked_stracks;
	 std::vector<STrack> lost_stracks;
	 std::vector<STrack> removed_stracks;
	byte_kalman::KalmanFilter kalman_filter;
};
