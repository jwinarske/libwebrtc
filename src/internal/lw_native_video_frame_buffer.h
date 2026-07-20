#ifndef LIB_WEBRTC_LW_NATIVE_VIDEO_FRAME_BUFFER_HXX
#define LIB_WEBRTC_LW_NATIVE_VIDEO_FRAME_BUFFER_HXX

#include "api/video/video_frame_buffer.h"
#include "c/lw_video_sink.h"

namespace libwebrtc {

// Shared native (zero-copy) frame buffer. The in-tree hardware decoder wraps a
// dmabuf frame in one of these; VideoSinkAdapter recognizes it and emits
// LwVideoSinkV1 callbacks instead of surfacing the frame to the CPU renderer
// path. type() is kNative and ToI420()/GetI420() yield null, so any accidental
// CPU access fails safe rather than reading back or dereferencing null.
//
// Ownership of the release obligation: the producer supplies
// release(release_ctx) which returns the dmabuf to the decoder pool. Exactly
// one release happens per buffer -- either a bound sink takes it over
// (DetachRelease) after accepting the frame, or this buffer releases on
// destruction.
class LwNativeVideoFrameBuffer : public webrtc::VideoFrameBuffer {
 public:
  LwNativeVideoFrameBuffer(const LwDmabufDescriptor& desc,
                           LwFrameRelease release, void* release_ctx);
  ~LwNativeVideoFrameBuffer() override;

  Type type() const override { return Type::kNative; }
  int width() const override { return static_cast<int>(desc_.width); }
  int height() const override { return static_cast<int>(desc_.height); }

  // No CPU pixels. Consumers must use descriptor(); this returns null so the
  // GetI420() base helper also yields null.
  webrtc::scoped_refptr<webrtc::I420BufferInterface> ToI420() override {
    return nullptr;
  }

  const LwDmabufDescriptor& descriptor() const { return desc_; }
  LwFrameRelease release_fn() const { return release_; }
  void* release_ctx() const { return release_ctx_; }

  // Transfers the release obligation to a consumer that returned nonzero from
  // on_frame; the destructor will no longer release.
  void DetachRelease() {
    release_ = nullptr;
    release_ctx_ = nullptr;
  }

 private:
  LwDmabufDescriptor desc_;
  LwFrameRelease release_;
  void* release_ctx_;
};

}  // namespace libwebrtc

#endif  // LIB_WEBRTC_LW_NATIVE_VIDEO_FRAME_BUFFER_HXX
