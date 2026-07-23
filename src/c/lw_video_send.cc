#include <cstdint>
#include <utility>
#include <vector>

#include "c/lw_c_api.h"
#include "rtc_mediaconstraints.h"
#include "rtc_peerconnection.h"
#include "rtc_peerconnection_factory.h"
#include "rtc_rtp_sender.h"
#include "rtc_video_frame.h"
#include "rtc_video_source.h"
#include "rtc_video_track.h"
#include "src/c/lw_handle.h"

using libwebrtc::RTCMediaConstraints;
using libwebrtc::RTCPeerConnection;
using libwebrtc::RTCPeerConnectionFactory;
using libwebrtc::RTCRtpSender;
using libwebrtc::RTCVideoFrame;
using libwebrtc::RTCVideoSource;
using libwebrtc::RTCVideoTrack;
using libwebrtc::scoped_refptr;
using libwebrtc::string;
using libwebrtc::vector;

extern "C" {

lw_video_source_t* lw_factory_create_video_source(lw_factory_t* factory,
                                                  const char* label) {
  auto* f = lw::From<RTCPeerConnectionFactory>(factory);
  if (f == nullptr) {
    return nullptr;
  }
  scoped_refptr<RTCMediaConstraints> constraints =
      RTCMediaConstraints::Create();
  scoped_refptr<RTCVideoSource> source = f->CreateCustomVideoSource(
      string(label != nullptr ? label : ""), constraints);
  return reinterpret_cast<lw_video_source_t*>(
      lw::Derive(factory, std::move(source)));
}

int lw_video_source_push_i420(lw_video_source_t* source, int width, int height,
                              const uint8_t* data, size_t size) {
  auto* s = lw::From<RTCVideoSource>(source);
  if (s == nullptr || data == nullptr || width <= 0 || height <= 0) {
    return -1;
  }
  // Reject anything whose size does not match the dimensions rather than
  // handing a short buffer to the frame constructor, which reads from it.
  const size_t expected =
      static_cast<size_t>(width) * static_cast<size_t>(height) * 3 / 2;
  if (size != expected) {
    return -1;
  }
  scoped_refptr<RTCVideoFrame> frame =
      RTCVideoFrame::Create(width, height, data, static_cast<int>(size));
  if (!frame.get()) {
    return -1;
  }
  s->OnCapturedFrame(frame);
  return 0;
}

lw_video_track_t* lw_factory_create_video_track(lw_factory_t* factory,
                                                lw_video_source_t* source,
                                                const char* id) {
  auto* f = lw::From<RTCPeerConnectionFactory>(factory);
  auto* s = lw::From<RTCVideoSource>(source);
  // An empty id is accepted by the track and then breaks negotiation, far from
  // here and with a message about the msid attribute, so reject it now.
  if (f == nullptr || s == nullptr || id == nullptr || *id == '\0') {
    return nullptr;
  }
  scoped_refptr<RTCVideoTrack> track =
      f->CreateVideoTrack(scoped_refptr<RTCVideoSource>(s), string(id));
  return reinterpret_cast<lw_video_track_t*>(
      lw::Derive(factory, std::move(track)));
}

lw_sender_t* lw_pc_add_track(lw_pc_t* pc, lw_video_track_t* track,
                             const char* const* stream_ids,
                             size_t stream_id_count) {
  auto* p = lw::From<RTCPeerConnection>(pc);
  auto* t = lw::From<RTCVideoTrack>(track);
  if (p == nullptr || t == nullptr ||
      (stream_ids == nullptr && stream_id_count != 0)) {
    return nullptr;
  }
  std::vector<string> ids;
  ids.reserve(stream_id_count);
  for (size_t i = 0; i < stream_id_count; ++i) {
    ids.push_back(string(stream_ids[i] != nullptr ? stream_ids[i] : ""));
  }
  scoped_refptr<RTCRtpSender> sender = p->AddTrack(
      scoped_refptr<libwebrtc::RTCMediaTrack>(t), vector<string>(ids));
  return reinterpret_cast<lw_sender_t*>(lw::Derive(pc, std::move(sender)));
}

int lw_video_track_set_enabled(lw_video_track_t* track, int enabled) {
  auto* t = lw::From<RTCVideoTrack>(track);
  if (t == nullptr) {
    return -1;
  }
  t->set_enabled(enabled != 0);
  return 0;
}

int lw_video_track_enabled(lw_video_track_t* track) {
  auto* t = lw::From<RTCVideoTrack>(track);
  if (t == nullptr) {
    return -1;
  }
  return t->enabled() ? 1 : 0;
}

}  // extern "C"
