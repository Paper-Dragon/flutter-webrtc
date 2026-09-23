#include "flutter_host_frame_source.h"

#include "rtc_mediaconstraints.h"
#include "rtc_peerconnection_factory.h"
#include "rtc_video_frame.h"

#include <tuple>

namespace flutter_webrtc_plugin {
namespace {

constexpr wchar_t kHostShmName[] = L"Local\\GeekeryRdHostFrames";
constexpr uint32_t kFrameMagic = 0x4D464452;

#pragma pack(push, 1)
struct FrameHeader {
  uint32_t magic;
  uint32_t width;
  uint32_t height;
  uint32_t stride;
  uint64_t seq;
  uint32_t ready;
  uint32_t flags;
};
#pragma pack(pop)

void BgraToI420(const uint8_t* bgra,
                int width,
                int height,
                int stride,
                std::vector<uint8_t>* y_plane,
                std::vector<uint8_t>* u_plane,
                std::vector<uint8_t>* v_plane) {
  const size_t y_size = static_cast<size_t>(width) * static_cast<size_t>(height);
  const size_t uv_w = static_cast<size_t>((width + 1) / 2);
  const size_t uv_h = static_cast<size_t>((height + 1) / 2);
  y_plane->resize(y_size);
  u_plane->resize(uv_w * uv_h);
  v_plane->resize(uv_w * uv_h);

  for (int j = 0; j < height; ++j) {
    const uint8_t* row = bgra + j * stride;
    uint8_t* yrow = y_plane->data() + j * width;
    for (int i = 0; i < width; ++i) {
      const uint8_t* p = row + i * 4;
      const int b = p[0];
      const int g = p[1];
      const int r = p[2];
      yrow[i] =
          static_cast<uint8_t>(((66 * r + 129 * g + 25 * b + 128) >> 8) + 16);
    }
  }

  for (int j = 0; j < height; j += 2) {
    const int j1 = (j + 1 < height) ? j + 1 : j;
    for (int i = 0; i < width; i += 2) {
      const int i1 = (i + 1 < width) ? i + 1 : i;
      auto at = [&](int x, int y) -> std::tuple<int, int, int> {
        const uint8_t* p = bgra + y * stride + x * 4;
        return {p[2], p[1], p[0]};
      };
      int r0, g0, b0, r1, g1, b1, r2, g2, b2, r3, g3, b3;
      std::tie(r0, g0, b0) = at(i, j);
      std::tie(r1, g1, b1) = at(i1, j);
      std::tie(r2, g2, b2) = at(i, j1);
      std::tie(r3, g3, b3) = at(i1, j1);
      const int r = (r0 + r1 + r2 + r3) / 4;
      const int g = (g0 + g1 + g2 + g3) / 4;
      const int b = (b0 + b1 + b2 + b3) / 4;
      const size_t uv_index =
          static_cast<size_t>(j / 2) * uv_w + static_cast<size_t>(i / 2);
      (*u_plane)[uv_index] = static_cast<uint8_t>(
          ((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128);
      (*v_plane)[uv_index] = static_cast<uint8_t>(
          ((112 * r - 94 * g - 18 * b + 128) >> 8) + 128);
    }
  }
}

}  // namespace

HostFramePump::HostFramePump() = default;

HostFramePump::~HostFramePump() { Stop(); }

bool HostFramePump::OpenShm() {
  if (view_) return true;
  mapping_ = OpenFileMappingW(FILE_MAP_READ, FALSE, kHostShmName);
  if (!mapping_) return false;
  view_ = MapViewOfFile(mapping_, FILE_MAP_READ, 0, 0, 0);
  if (!view_) {
    CloseHandle(mapping_);
    mapping_ = nullptr;
    return false;
  }
  return true;
}

void HostFramePump::CloseShm() {
  if (view_) {
    UnmapViewOfFile(view_);
    view_ = nullptr;
  }
  if (mapping_) {
    CloseHandle(mapping_);
    mapping_ = nullptr;
  }
}

bool HostFramePump::Start(scoped_refptr<RTCVideoSource> source) {
  Stop();
  if (!source.get()) return false;
  source_ = source;
  running_ = true;
  thread_ = CreateThread(nullptr, 0, ThreadMain, this, 0, nullptr);
  if (!thread_) {
    running_ = false;
    source_ = nullptr;
    return false;
  }
  return true;
}

void HostFramePump::Stop() {
  running_ = false;
  if (thread_) {
    WaitForSingleObject(thread_, 3000);
    CloseHandle(thread_);
    thread_ = nullptr;
  }
  CloseShm();
  source_ = nullptr;
  last_seq_ = 0;
}

DWORD WINAPI HostFramePump::ThreadMain(LPVOID self) {
  static_cast<HostFramePump*>(self)->Loop();
  return 0;
}

void HostFramePump::Loop() {
  while (running_.load()) {
    if (!OpenShm()) {
      Sleep(50);
      continue;
    }
    auto* hdr = static_cast<const FrameHeader*>(view_);
    if (!hdr || hdr->magic != kFrameMagic || !hdr->ready || hdr->width == 0 ||
        hdr->height == 0) {
      Sleep(16);
      continue;
    }
    if (hdr->seq == last_seq_) {
      Sleep(8);
      continue;
    }
    last_seq_ = hdr->seq;
    const int w = static_cast<int>(hdr->width);
    const int h = static_cast<int>(hdr->height);
    const int stride = static_cast<int>(hdr->stride);
    const uint8_t* bgra =
        static_cast<const uint8_t*>(view_) + sizeof(FrameHeader);
    BgraToI420(bgra, w, h, stride, &y_, &u_, &v_);
    if (source_.get()) {
      auto frame = RTCVideoFrame::Create(w, h, y_.data(), w, u_.data(),
                                         (w + 1) / 2, v_.data(), (w + 1) / 2);
      if (frame.get()) {
        source_->OnCapturedFrame(frame);
      }
    }
    Sleep(8);
  }
}

FlutterHostFrameSource::FlutterHostFrameSource(FlutterWebRTCBase* base)
    : base_(base) {}

void FlutterHostFrameSource::CreateCustomVideoTrack(
    std::unique_ptr<MethodResultProxy> result) {
  base_->EnsureWebRTCInitialized();
  auto constraints = RTCMediaConstraints::Create();
  scoped_refptr<RTCVideoSource> source =
      base_->factory_->CreateCustomVideoSource("host_frames", constraints);
  if (!source.get()) {
    result->Error("createCustomVideoTrack", "CreateCustomVideoSource failed");
    return;
  }
  std::string uuid = base_->GenerateUUID();
  scoped_refptr<RTCVideoTrack> track =
      base_->factory_->CreateVideoTrack(source, uuid.c_str());
  if (!track.get()) {
    result->Error("createCustomVideoTrack", "CreateVideoTrack failed");
    return;
  }
  scoped_refptr<RTCMediaStream> stream =
      base_->factory_->CreateStream(base_->GenerateUUID().c_str());
  stream->AddTrack(track);
  base_->local_tracks_[track->id().std_string()] = track;
  base_->local_streams_[stream->id().std_string()] = stream;
  custom_sources_[track->id().std_string()] = source;

  EncodableMap params;
  EncodableList videoTracks;
  EncodableMap info;
  info[EncodableValue("id")] = EncodableValue(track->id().std_string());
  info[EncodableValue("label")] = EncodableValue(track->id().std_string());
  info[EncodableValue("kind")] = EncodableValue(track->kind().std_string());
  info[EncodableValue("enabled")] = EncodableValue(track->enabled());
  videoTracks.push_back(EncodableValue(info));
  params[EncodableValue("streamId")] =
      EncodableValue(stream->id().std_string());
  params[EncodableValue("videoTracks")] = EncodableValue(videoTracks);
  result->Success(EncodableValue(params));
}

void FlutterHostFrameSource::StartHostShmCapture(
    const std::string& track_id,
    std::unique_ptr<MethodResultProxy> result) {
  auto it = custom_sources_.find(track_id);
  if (it == custom_sources_.end()) {
    result->Error("startHostShmCapture", "custom source not found");
    return;
  }
  if (!pump_) pump_ = std::make_unique<HostFramePump>();
  if (!pump_->Start(it->second)) {
    result->Error("startHostShmCapture", "failed to start pump");
    return;
  }
  pumping_track_id_ = track_id;
  result->Success(EncodableValue(true));
}

void FlutterHostFrameSource::StopHostShmCapture(
    std::unique_ptr<MethodResultProxy> result) {
  if (pump_) pump_->Stop();
  pumping_track_id_.clear();
  result->Success(EncodableValue(true));
}

}  // namespace flutter_webrtc_plugin
