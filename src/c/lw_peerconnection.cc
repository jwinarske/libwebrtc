#include <utility>

#include "c/lw_c_api.h"
#include "rtc_mediaconstraints.h"
#include "rtc_peerconnection.h"
#include "rtc_peerconnection_factory.h"
#include "rtc_rtp_receiver.h"
#include "rtc_rtp_transceiver.h"
#include "rtc_types.h"
#include "rtc_video_track.h"
#include "src/c/lw_handle.h"

using libwebrtc::RTCConfiguration;
using libwebrtc::RTCMediaConstraints;
using libwebrtc::RTCMediaTrack;
using libwebrtc::RTCMediaType;
using libwebrtc::RTCPeerConnection;
using libwebrtc::RTCPeerConnectionFactory;
using libwebrtc::RTCRtpReceiver;
using libwebrtc::RTCRtpTransceiver;
using libwebrtc::RTCVideoTrack;
using libwebrtc::scoped_refptr;

namespace {

RTCMediaType ToRtcMediaType(lw_media_type type) {
  switch (type) {
    case LW_MEDIA_AUDIO:
      return RTCMediaType::AUDIO;
    case LW_MEDIA_VIDEO:
      return RTCMediaType::VIDEO;
    case LW_MEDIA_DATA:
      return RTCMediaType::DATA;
  }
  return RTCMediaType::UNSUPPORTED;
}

}  // namespace

extern "C" {

lw_pc_t* lw_pc_create(lw_factory_t* factory) {
  auto* f = lw::From<RTCPeerConnectionFactory>(factory);
  if (!f) {
    return nullptr;
  }
  RTCConfiguration config;
  scoped_refptr<RTCMediaConstraints> constraints =
      RTCMediaConstraints::Create();
  scoped_refptr<RTCPeerConnection> pc = f->Create(config, constraints);
  if (!pc.get()) {
    return nullptr;
  }
  return reinterpret_cast<lw_pc_t*>(lw::Derive(factory, std::move(pc)));
}

void lw_pc_close(lw_pc_t* pc) {
  if (auto* p = lw::From<RTCPeerConnection>(pc)) {
    p->Close();
  }
}

lw_transceiver_t* lw_pc_add_transceiver(lw_pc_t* pc, lw_media_type media_type) {
  auto* p = lw::From<RTCPeerConnection>(pc);
  if (!p) {
    return nullptr;
  }
  scoped_refptr<RTCRtpTransceiver> transceiver =
      p->AddTransceiver(ToRtcMediaType(media_type));
  if (!transceiver.get()) {
    return nullptr;
  }
  return reinterpret_cast<lw_transceiver_t*>(
      lw::Derive(pc, std::move(transceiver)));
}

lw_receiver_t* lw_transceiver_receiver(lw_transceiver_t* transceiver) {
  auto* t = lw::From<RTCRtpTransceiver>(transceiver);
  if (!t) {
    return nullptr;
  }
  scoped_refptr<RTCRtpReceiver> receiver = t->receiver();
  if (!receiver.get()) {
    return nullptr;
  }
  return reinterpret_cast<lw_receiver_t*>(
      lw::Derive(transceiver, std::move(receiver)));
}

lw_video_track_t* lw_receiver_video_track(lw_receiver_t* receiver) {
  auto* r = lw::From<RTCRtpReceiver>(receiver);
  if (!r) {
    return nullptr;
  }
  scoped_refptr<RTCMediaTrack> track = r->track();
  if (!track.get()) {
    return nullptr;
  }
  auto* video = dynamic_cast<RTCVideoTrack*>(track.get());
  if (!video) {
    return nullptr;  // not a video track
  }
  return reinterpret_cast<lw_video_track_t*>(
      lw::Derive(receiver, scoped_refptr<RTCVideoTrack>(video)));
}

}  // extern "C"
