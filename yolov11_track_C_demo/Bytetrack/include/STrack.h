#pragma once

#include <opencv2/opencv.hpp>
#include "kalmanFilter.h"

enum TrackState { New = 0, Tracked, Lost, Removed };

class STrack
{
public:
	STrack( std::vector<float> tlwh_, float score);
	~STrack();

	 std::vector<float> static tlbr_to_tlwh( std::vector<float> &tlbr);
	void static multi_predict( std::vector<STrack*> &stracks, byte_kalman::KalmanFilter &kalman_filter);
	void static_tlwh();
	void static_tlbr();
	 std::vector<float> tlwh_to_xyah( std::vector<float> tlwh_tmp);
	 std::vector<float> to_xyah();
	void mark_lost();
	void mark_removed();
	int next_id();
	int end_frame();
	
	void activate(byte_kalman::KalmanFilter &kalman_filter, int frame_id);
	void re_activate(STrack &new_track, int frame_id, bool new_id = false);
	void update(STrack &new_track, int frame_id);

public:
	bool is_activated;				// 该跟踪对象是否被激活：true该目标正在被跟踪，false该目标处于丢失状态。
	int track_id;					// 该跟踪对象的唯一ID
	int state;						// 表示跟踪对象的状态，TrackState枚举

	 std::vector<float> _tlwh;		// 原始目标框
	 std::vector<float> tlwh;		// 修正后的目标框
	 std::vector<float> tlbr;		// 便于IOU计算的目标框
	int frame_id;					// 当前目标最后一次被更新（或检测到）的帧ID
	int tracklet_len;				// 该目标已被连续跟踪的帧数
	int start_frame;				// 该目标首次被检测到的帧ID

	KAL_MEAN mean;					// 卡尔曼滤波的状态向量
	KAL_COVA covariance;			// 卡尔曼滤波的协方差矩阵
	float score;					// 该目标的置信度分数
		
private:
	byte_kalman::KalmanFilter kalman_filter;
};