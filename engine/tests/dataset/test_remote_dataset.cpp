#include "control/remote_dataset.h"
#include "runtime/app_ctrl.h"
#include "cJSON.h"
#include <cassert>
#include <chrono>
#include <fstream>
#include <memory>
#include <thread>
#include <csignal>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static std::shared_ptr<AppRuntimeSnapshot> runtime;
std::shared_ptr<const AppRuntimeSnapshot> app_ctrl_get_runtime_snapshot() { return runtime; }
const ChannelConfig *app_ctrl_runtime_channel_config(const std::shared_ptr<const AppRuntimeSnapshot> &snapshot, int channel) {
    if (snapshot) for (const auto &config : snapshot->config.channels) if (config.id==channel) return &config;
    return nullptr;
}
const cv::Mat *ChannelContext::source_frame() const { return source_frame_getter ? source_frame_getter(frame_getter_opaque) : nullptr; }
const RoiZone *ChannelContext::roi_by_name(const char *name) const {
    if (rois) for (const auto &roi : *rois) if (roi.name==name) return &roi;
    return nullptr;
}
using Json = std::unique_ptr<cJSON,decltype(&cJSON_Delete)>;
static Json command(const std::string &value, std::vector<unsigned char> *bytes=nullptr) {
    std::vector<unsigned char> ignored;
    auto result=remote_dataset_command(value,bytes?*bytes:ignored);
    return Json(cJSON_Parse(result.c_str()),cJSON_Delete);
}
static const cJSON *item(const cJSON *root, const char *key) { return cJSON_GetObjectItemCaseSensitive(root,key); }
static const cJSON *task(const cJSON *root, int channel) {
    auto list=item(root,"tasks");
    for(auto v=list->child;v;v=v->next) if(item(v,"channel_id")->valueint==channel) return v;
    return nullptr;
}
static uint64_t now() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
static std::string configure(int ch, bool enabled=true, int limit=0, const std::string &label="person") {
    return "{\"op\":\"configure\",\"task_id\":\"task"+std::to_string(ch)+"\",\"revision\":\"v1\",\"channel_id\":"+std::to_string(ch)+
      ",\"enabled\":"+(enabled?"true":"false")+",\"interval_sec\":0.5,\"max_samples\":"+std::to_string(limit)+
      ",\"rules\":[{\"id\":\"r1\",\"name\":\"test\",\"condition\":{\"class\":\""+label+"\",\"min_confidence\":0.3,\"count\":{\"op\":\">=\",\"value\":1}}}]}";
}
static const cv::Mat *get_frame(void *value) { return static_cast<cv::Mat *>(value); }
static void observe(int channel, int64_t seq, bool valid=true, bool infer=true) {
    cv::Mat frame(240,320,CV_8UC3,cv::Scalar(10,90,200));
    std::vector<AlgoResult> results(1);
    results[0].label="person";results[0].score=.9;results[0].frame_id=seq;results[0].model_id="model_0";
    results[0].box={10,10,20,50};results[0].box_color=cv::Scalar(5,6,7);
    ChannelContext context{};
    context.chnId=channel;context.frame_id=seq;context.timestamp_ms=now();context.unix_ms=1700000000000ULL+seq;
    context.inference_valid=valid;context.infer_enabled=infer;context.results=&results;
    context.src_width=320;context.src_height=240;context.source_frame_getter=get_frame;context.frame_getter_opaque=&frame;
    remote_dataset_observe(context,runtime);
    assert(results[0].box_color==cv::Scalar(5,6,7)); // Independent observer never changes business results.
}
static Json wait_pending(int ch,int count) {
    for(int i=0;i<100;++i) {
        auto result=command("{\"op\":\"status\"}");
        const auto entry=task(result.get(),ch);
        if(entry && item(entry,"pending")->valueint==count && (count==0||item(entry,"next"))) return result;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    assert(false);return Json(nullptr,cJSON_Delete);
}
static volatile std::sig_atomic_t keep_serving=1;
int main(int argc, char **argv) {
    char pattern[]="/tmp/dataset-remote-test-XXXXXX";
    const std::string dir=mkdtemp(pattern),path=argc>1?argv[1]:dir+"/run.dataset.sock";
    std::ofstream(dir+"/labels.txt") << "person\nhelmet\n";
    runtime=std::make_shared<AppRuntimeSnapshot>();
    for(int ch : {0,3}) {
        ChannelConfig config;config.id=ch;
        ChannelModelConfig model;model.enable=true;model.model_type="yolov8_det";model.label_path=dir+"/labels.txt";model.obj_thresh=.3;
        config.models.push_back(model);runtime->config.channels.push_back(config);
    }
    assert(remote_dataset_init(path)==0);
    if (argc>1) {
        std::signal(SIGTERM,[](int){keep_serving=0;});
        int64_t frame=0;
        while (keep_serving) {
            ++frame;
            for(int channel : {0,3}) if(remote_dataset_active(channel)) observe(channel,frame);
            std::this_thread::sleep_for(std::chrono::milliseconds(520));
        }
        remote_dataset_deinit();unlink((dir+"/labels.txt").c_str());rmdir(dir.c_str());return 0;
    }
    auto result=command(configure(0,true,2)); assert(cJSON_IsTrue(item(result.get(),"ok")));
    result=command(configure(3)); assert(cJSON_IsTrue(item(result.get(),"ok")));
    assert(remote_dataset_active(0)&&remote_dataset_active(3)&&!remote_dataset_active(1));
    observe(0,1,false);observe(3,1,true,false);
    result=command("{\"op\":\"status\"}");
    assert(item(task(result.get(),0),"pending")->valueint==0&&item(task(result.get(),3),"pending")->valueint==0);
    observe(0,2);observe(3,2);
    result=wait_pending(0,1);wait_pending(3,1);
    const auto next=item(task(result.get(),0),"next");
    const std::string id=item(next,"sample_id")->valuestring;
    assert(item(next,"frame_id")->valueint==2);
    std::vector<unsigned char> jpeg;
    result=command("{\"op\":\"image\",\"task_id\":\"task0\",\"sample_id\":\""+id+"\"}",&jpeg);
    auto decoded=cv::imdecode(jpeg,cv::IMREAD_COLOR);assert(decoded.size()==cv::Size(320,240));
    auto pixel=decoded.at<cv::Vec3b>(15,15);assert(std::abs(pixel[0]-10)<4&&std::abs(pixel[2]-200)<4);
    // Download is repeatable and does not release/count images.
    result=command("{\"op\":\"status\"}");assert(item(task(result.get(),0),"saved")->valueint==0);
    const std::string ack="{\"op\":\"ack\",\"task_id\":\"task0\",\"sample_id\":\""+id+"\",\"saved\":1}";
    command(ack);command(ack);
    result=wait_pending(0,0);assert(item(task(result.get(),0),"saved")->valueint==1);
    assert(item(task(result.get(),3),"pending")->valueint==1);
    std::this_thread::sleep_for(std::chrono::milliseconds(510));
    observe(0,3);result=wait_pending(0,1);
    std::this_thread::sleep_for(std::chrono::milliseconds(510));
    observe(0,4); // saved+pending enforces max=2 before PC ACK.
    result=command("{\"op\":\"status\"}");assert(item(task(result.get(),0),"pending")->valueint==1);
    auto invalid=configure(3);auto pos=invalid.find("\"v1\"");invalid.replace(pos,4,"\"v2\"");
    result=command(invalid);assert(cJSON_IsFalse(item(result.get(),"ok"))); // No discard when editing a pending task.
    for(int i=0;i<9;++i) {std::this_thread::sleep_for(std::chrono::milliseconds(510));observe(3,10+i);}
    result=wait_pending(3,8);assert(item(task(result.get(),3),"skipped")->valueint>=1);
    result=command(configure(3,false));assert(cJSON_IsTrue(item(result.get(),"ok"))&&!remote_dataset_active(3));
    result=command("{\"op\":\"status\"}");assert(item(task(result.get(),3),"pending")->valueint==8);
    // Fragmented IPC request, independent socket, bounded framing.
    int fd=socket(AF_UNIX,SOCK_STREAM,0);sockaddr_un address{};address.sun_family=AF_UNIX;
    std::copy(path.begin(),path.end(),address.sun_path);
    assert(connect(fd,reinterpret_cast<sockaddr *>(&address),sizeof(address))==0);
    assert(send(fd,"{\"op\":",6,0)==6);assert(send(fd,"\"status\"}\n",10,0)==10);
    char buffer[8192];int bytes=recv(fd,buffer,sizeof(buffer),0);assert(bytes>0);close(fd);
    remote_dataset_deinit();assert(!remote_dataset_active(0));
    assert(remote_dataset_init(path)==0);
    invalid=configure(0,true,0,"unknown");result=command(invalid);assert(cJSON_IsFalse(item(result.get(),"ok")));
    remote_dataset_deinit();unlink((dir+"/labels.txt").c_str());rmdir(dir.c_str());
    puts("Multi-channel capture, raw JPEG, ACK/retry, quota, bounded queue, pause and IPC tests passed");
}
