#include "rtc_video_sink_adapter.h"

#include "rtc_base/logging.h"
#include "rtc_video_frame_impl.h"
#include "rtc_video_track.h"
#include "src/internal/lw_native_video_frame_buffer.h"

namespace libwebrtc {

VideoSinkAdapter::VideoSinkAdapter(
    webrtc::scoped_refptr<webrtc::VideoTrackInterface> track)
    : rtc_track_(track), crt_sec_(new webrtc::Mutex()) {
  rtc_track_->AddOrUpdateSink(this, webrtc::VideoSinkWants());
  RTC_LOG(LS_INFO) << __FUNCTION__ << ": ctor " << (void*)this;
}

VideoSinkAdapter::~VideoSinkAdapter() {
  rtc_track_->RemoveSink(this);
  RTC_LOG(LS_INFO) << __FUNCTION__ << ": dtor ";
}

// VideoSinkInterface implementation
void VideoSinkAdapter::OnFrame(const webrtc::VideoFrame& video_frame) {
  webrtc::MutexLock cs(crt_sec_.get());

  // Telemetry observer: every decoded frame delivered to this adapter, native
  // or CPU, is counted here before the native/CPU split.
  if (frame_cb_) {
    frame_cb_(video_frame.width(), video_frame.height(), frame_user_);
  }

  // Native (zero-copy) frames go to the bound native sink and must never reach
  // the CPU renderer path.
  if (DeliverNative(video_frame)) {
    return;
  }

  scoped_refptr<VideoFrameBufferImpl> frame_buffer =
      scoped_refptr<VideoFrameBufferImpl>(
          new RefCountedObject<VideoFrameBufferImpl>(
              video_frame.video_frame_buffer()));

  frame_buffer->set_rotation(video_frame.rotation());
  frame_buffer->set_timestamp_us(video_frame.timestamp_us());

  for (auto renderer : renderers_) {
    renderer->OnFrame(frame_buffer);
  }
}

bool VideoSinkAdapter::DeliverNative(const webrtc::VideoFrame& video_frame) {
  const webrtc::scoped_refptr<webrtc::VideoFrameBuffer> buffer =
      video_frame.video_frame_buffer();
  if (!buffer || buffer->type() != webrtc::VideoFrameBuffer::Type::kNative) {
    return false;  // CPU frame: let the caller run the renderer path.
  }

  // kNative but not our shared class (some other native buffer): still not for
  // the CPU renderer -- drop rather than force a readback.
  auto* native = dynamic_cast<LwNativeVideoFrameBuffer*>(buffer.get());
  if (!native) {
    return true;
  }

  if (native_sink_ && native_sink_->on_frame) {
    const LwDmabufDescriptor& desc = native->descriptor();
    // on_format precedes the first frame of each pool generation.
    if (!have_generation_ || desc.pool_generation != last_generation_) {
      if (native_sink_->on_format) {
        native_sink_->on_format(&desc, native_user_);
      }
      last_generation_ = desc.pool_generation;
      have_generation_ = true;
    }
    const int taken = native_sink_->on_frame(
        &desc, native->release_fn(), native->release_ctx(), native_user_);
    if (taken) {
      // The sink owns the release obligation now; the buffer must not also
      // release when it is destroyed.
      native->DetachRelease();
    }
  }
  // No sink bound, or the sink declined: the buffer releases on destruction.
  return true;
}

void VideoSinkAdapter::SetNativeSink(const LwVideoSinkV1* sink, void* user) {
  webrtc::MutexLock cs(crt_sec_.get());
  if (native_sink_ && native_sink_ != sink && native_sink_->on_eos) {
    native_sink_->on_eos(native_user_);
  }
  native_sink_ = sink;
  native_user_ = user;
  have_generation_ = false;  // re-announce format to the new binding
}

void VideoSinkAdapter::SetFrameObserver(void (*cb)(int, int, void*),
                                        void* user) {
  webrtc::MutexLock cs(crt_sec_.get());
  frame_cb_ = cb;
  frame_user_ = user;
}

void VideoSinkAdapter::AddRenderer(
    RTCVideoRenderer<scoped_refptr<RTCVideoFrame>>* renderer) {
  RTC_LOG(LS_INFO) << __FUNCTION__ << ": AddRenderer " << (void*)renderer;
  webrtc::MutexLock cs(crt_sec_.get());
  renderers_.push_back(renderer);
}

void VideoSinkAdapter::RemoveRenderer(
    RTCVideoRenderer<scoped_refptr<RTCVideoFrame>>* renderer) {
  RTC_LOG(LS_INFO) << __FUNCTION__ << ": RemoveRenderer " << (void*)renderer;
  webrtc::MutexLock cs(crt_sec_.get());
  renderers_.erase(
      std::remove_if(
          renderers_.begin(), renderers_.end(),
          [renderer](
              const RTCVideoRenderer<scoped_refptr<RTCVideoFrame>>* renderer_) {
            return renderer_ == renderer;
          }),
      renderers_.end());
}

void VideoSinkAdapter::AddRenderer(
    webrtc::VideoSinkInterface<webrtc::VideoFrame>* renderer) {
  rtc_track_->AddOrUpdateSink(renderer, webrtc::VideoSinkWants());
}
void VideoSinkAdapter::RemoveRenderer(
    webrtc::VideoSinkInterface<webrtc::VideoFrame>* renderer) {
  rtc_track_->RemoveSink(renderer);
}

}  // namespace libwebrtc
