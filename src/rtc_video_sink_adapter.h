#ifndef LIB_WEBRTC_VIDEO_SINK_ADPTER_HXX
#define LIB_WEBRTC_VIDEO_SINK_ADPTER_HXX

#include "api/media_stream_interface.h"
#include "api/peer_connection_interface.h"
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
};

}  // namespace libwebrtc

#endif  // LIB_WEBRTC_VIDEO_SINK_ADPTER_HXX
