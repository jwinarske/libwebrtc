#include "src/internal/lw_native_video_frame_buffer.h"

namespace libwebrtc {

LwNativeVideoFrameBuffer::LwNativeVideoFrameBuffer(
    const LwDmabufDescriptor& desc, LwFrameRelease release, void* release_ctx)
    : desc_(desc), release_(release), release_ctx_(release_ctx) {}

webrtc::VideoFrameBuffer::Type LwNativeVideoFrameBuffer::type() const {
  return Type::kNative;
}

int LwNativeVideoFrameBuffer::width() const {
  return static_cast<int>(desc_.width);
}

int LwNativeVideoFrameBuffer::height() const {
  return static_cast<int>(desc_.height);
}

webrtc::scoped_refptr<webrtc::I420BufferInterface>
LwNativeVideoFrameBuffer::ToI420() {
  return nullptr;
}

LwNativeVideoFrameBuffer::~LwNativeVideoFrameBuffer() {
  // Release only if no consumer took the obligation over (DetachRelease).
  if (release_) {
    release_(release_ctx_);
  }
}

}  // namespace libwebrtc
