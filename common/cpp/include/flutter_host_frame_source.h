#ifndef FLUTTER_HOST_FRAME_SOURCE_HXX
#define FLUTTER_HOST_FRAME_SOURCE_HXX

#include "flutter_common.h"
#include "flutter_webrtc_base.h"

#include "rtc_types.h"
#include "rtc_video_source.h"
#include "rtc_video_track.h"

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <windows.h>

namespace flutter_webrtc_plugin {

using namespace libwebrtc;

class HostFramePump {
 public:
  HostFramePump();
  ~HostFramePump();

  bool Start(scoped_refptr<RTCVideoSource> source);
  void Stop();
  bool running() const { return running_.load(); }

 private:
  static DWORD WINAPI ThreadMain(LPVOID self);
  void Loop();
  bool OpenShm();
  void CloseShm();

  std::atomic<bool> running_{false};
  HANDLE thread_ = nullptr;
  scoped_refptr<RTCVideoSource> source_;
  HANDLE mapping_ = nullptr;
  void* view_ = nullptr;
  uint64_t last_seq_ = 0;
  std::vector<uint8_t> y_;
  std::vector<uint8_t> u_;
  std::vector<uint8_t> v_;
};

class FlutterHostFrameSource {
 public:
  explicit FlutterHostFrameSource(FlutterWebRTCBase* base);

  void CreateCustomVideoTrack(std::unique_ptr<MethodResultProxy> result);
  void StartHostShmCapture(const std::string& track_id,
                           std::unique_ptr<MethodResultProxy> result);
  void StopHostShmCapture(std::unique_ptr<MethodResultProxy> result);

 private:
  FlutterWebRTCBase* base_;
  std::map<std::string, scoped_refptr<RTCVideoSource>> custom_sources_;
  std::unique_ptr<HostFramePump> pump_;
  std::string pumping_track_id_;
};

}  // namespace flutter_webrtc_plugin

#endif
