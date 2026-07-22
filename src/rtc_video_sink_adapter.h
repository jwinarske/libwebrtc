#ifndef LIB_WEBRTC_VIDEO_SINK_ADPTER_HXX
#define LIB_WEBRTC_VIDEO_SINK_ADPTER_HXX

#include <atomic>

#include "api/media_stream_interface.h"
#include "api/peer_connection_interface.h"
#include "c/lw_c_api.h"
#include "c/lw_video_sink.h"
#include "rtc_base/synchronization/mutex.h"
#include "rtc_peerconnection.h"
#include "rtc_video_frame.h"

namespace libwebrtc {

class VideoSinkAdapter : public webrtc::VideoSinkInterface<webrtc::VideoFrame>,
                         public RefCountInterface {
 public:
  VideoSinkAdapter(webrtc::scoped_refptr<webrtc::VideoTrackInterface> track);
  ~VideoSinkAdapter() override;

  virtual void AddRenderer(
      RTCVideoRenderer<scoped_refptr<RTCVideoFrame>>* renderer);

  virtual void RemoveRenderer(
      RTCVideoRenderer<scoped_refptr<RTCVideoFrame>>* renderer);

  virtual void AddRenderer(
      webrtc::VideoSinkInterface<webrtc::VideoFrame>* renderer);

  virtual void RemoveRenderer(
      webrtc::VideoSinkInterface<webrtc::VideoFrame>* renderer);

  // Binds a native (zero-copy) sink. When set, frames whose buffer is the
  // shared native class are delivered to `sink` via LwVideoSinkV1 callbacks
  // instead of the CPU renderer path; such frames never reach the C++
  // renderer. Pass nullptr to unbind (emits on_eos to the previously-bound
  // sink). Ownership of `sink`/`user` stays with the caller and must outlive
  // the binding; unbinding quiesces (waits for any in-flight on_frame).
  virtual void SetNativeSink(const LwVideoSinkV1* sink, void* user);

  // Sets a lightweight per-frame observer that reports each delivered frame's
  // dimensions (counting/telemetry). Fires for both native and CPU frames.
  virtual void SetFrameObserver(void (*cb)(int, int, void*), void* user);

  // Fills `out` with this adapter's frame counters. Safe to call at frame
  // rate: the counters are atomics, read without taking crt_sec_, so a reader
  // never contends with the delivery path.
  virtual void GetStats(LwVideoTrackStats* out) const;

 protected:
  // VideoSinkInterface implementation
  void OnFrame(const webrtc::VideoFrame& frame) override;

  // Routes a native-buffer frame to native_sink_. Returns true if the frame
  // was a native buffer (handled here, must not fall through to the CPU
  // renderer path). Caller holds crt_sec_.
  bool DeliverNative(const webrtc::VideoFrame& frame);

  webrtc::scoped_refptr<webrtc::VideoTrackInterface> rtc_track_;
  std::unique_ptr<webrtc::Mutex> crt_sec_;
  std::vector<RTCVideoRenderer<scoped_refptr<RTCVideoFrame>>*> renderers_;

  // Native (zero-copy) sink binding, guarded by crt_sec_.
  const LwVideoSinkV1* native_sink_ = nullptr;
  void* native_user_ = nullptr;
  uint32_t last_generation_ = 0;
  bool have_generation_ = false;

  // Per-frame telemetry observer, guarded by crt_sec_.
  void (*frame_cb_)(int, int, void*) = nullptr;
  void* frame_user_ = nullptr;

  // Counters. Written only on the delivery thread and read from anywhere, so
  // relaxed ordering is enough: each is independently monotonic and a reader
  // that catches a set mid-update sees a slightly stale count, not a torn one.
  std::atomic<uint64_t> frames_delivered_{0};
  std::atomic<uint64_t> frames_native_{0};
  std::atomic<uint64_t> frames_cpu_{0};
  std::atomic<uint64_t> frames_dropped_{0};
  std::atomic<uint32_t> last_width_{0};
  std::atomic<uint32_t> last_height_{0};
  std::atomic<int64_t> last_frame_us_{0};
};

}  // namespace libwebrtc

#endif  // LIB_WEBRTC_VIDEO_SINK_ADPTER_HXX
