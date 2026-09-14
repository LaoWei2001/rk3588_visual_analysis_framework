#include <stdio.h>
#include "opencv2/opencv.hpp"
#include "yolo11.h"
#include "BYTETracker.h"

/// yolo检测框转跟踪对象
void decobj_to_trackobj(std::vector<rknn_yolo11_result_t> &objects, std::vector<Object> &trackobj)
{
	// 如果objects不为空，初始化trackobj
	if (!objects.empty())
	{
		trackobj.clear();
	}
	// 将objects转换为trackobj
	for (auto &obj : objects)
	{
		// 新建Object对象
		Object trackobj_temp;
		trackobj_temp.classId = obj.cls_id;
		trackobj_temp.score = obj.prop;

		int x = obj.left;
		int y = obj.top;
		int w = obj.right - obj.left;
		int h = obj.bottom - obj.top;
		trackobj_temp.box = cv::Rect(x, y, w, h);
		trackobj.push_back(trackobj_temp);
	}

}


/// 主函数
int main(int argc, char **argv)
{
	if (argc < 3) {
		printf("Usage: %s <model_path> <video_path>\n", argv[0]);
		return -1;
	}
	int ret;
	char *p_model_path = argv[1];
	char *p_video_path = argv[2];
	printf("Model path = %s, video path = %s\n\n", p_model_path, p_video_path);
	cv::VideoCapture video(p_video_path, cv::CAP_FFMPEG);
	if (!video.isOpened()) {
		std::cerr << "Error: Could not open video source." << std::endl;
		return -1;
	}

	int width = int(video.get(cv::CAP_PROP_FRAME_WIDTH));
	int height = int(video.get(cv::CAP_PROP_FRAME_HEIGHT));
	int fps = int(video.get(cv::CAP_PROP_FPS));
	cv::VideoWriter writer("output.avi", cv::VideoWriter::fourcc('M', 'J', 'P', 'G'), fps, cv::Size(width, height));

	rknn_yolo11_context_t yolo11;
	memset(&yolo11, 0, sizeof(yolo11));
	ret = yolov11_detect_init(p_model_path, &yolo11);

	std::vector<Object> trackobj;
	std::vector<STrack> output_stracks;
	static  BYTETracker tracker(0.35, 0.6, 0.6, 30, 30);

	cv::Mat frame;
	while (true)
	{
		bool ret = video.read(frame);
		if (!ret || frame.empty()) {
			break;
		}
		double start_time = static_cast<double>(cv::getTickCount());

		std::vector<rknn_yolo11_result_t> results = yolov11_detect_run(frame, &yolo11, 0.35, 0.35);

		double yolo11_time = static_cast<double>(cv::getTickCount());

		// 将检测对象转换成追踪对象
		decobj_to_trackobj(results, trackobj);
		output_stracks = tracker.update(trackobj);

		double total_time = static_cast<double>(cv::getTickCount());
		double yolo11_elapsed = (yolo11_time - start_time) / cv::getTickFrequency() * 1000;
		double total_elapsed = (total_time - start_time) / cv::getTickFrequency() * 1000;

		std::cout << "Yolo11 run time: " << yolo11_elapsed << " ms, Bytetrack time: " << total_elapsed - yolo11_elapsed
			<< "ms, Total time: " << total_elapsed << "ms\n";

		for (unsigned long i = 0; i < output_stracks.size(); i++)
		{
			std::vector<float> tlwh = output_stracks[i].tlwh;
			cv::Scalar s = tracker.get_color(output_stracks[i].track_id);
			cv::putText(frame, cv::format("%d", output_stracks[i].track_id), cv::Point(tlwh[0], tlwh[1] - 5),
				0, 0.6, cv::Scalar(0, 0, 255), 2, cv::LINE_AA);
			cv::rectangle(frame, cv::Rect(tlwh[0], tlwh[1], tlwh[2], tlwh[3]), s, 2);
			
		}

		writer.write(frame);

	}

	writer.release();
	
	yolov11_detect_release(&yolo11);
	
	return 0;
}
