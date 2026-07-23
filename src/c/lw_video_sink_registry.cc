#include <cstdint>
#include <map>
#include <random>
#include <utility>

#include "c/lw_c_api.h"
#include "rtc_base/synchronization/mutex.h"
#include "src/c/lw_handle.h"
#include "src/c/lw_string.h"
#include "src/rtc_video_track_impl.h"

namespace {

struct SinkEntry {
  const LwVideoSinkV1* sink = nullptr;
  void* user = nullptr;
};

// Process-wide sink registry. Tokens are unguessable u64 handles (not
// well-known values) as cheap hardening against in-process track redirection.
webrtc::Mutex& RegistryMutex() {
  static webrtc::Mutex* m = new webrtc::Mutex();
  return *m;
}

std::map<lw_video_sink_token, SinkEntry>& Registry() {
  static std::map<lw_video_sink_token, SinkEntry>* r =
      new std::map<lw_video_sink_token, SinkEntry>();
  return *r;
}

// Caller must hold RegistryMutex(). Returns a nonzero token not already in use.
lw_video_sink_token GenerateToken() {
  static std::random_device rd;
  lw_video_sink_token token = 0;
  do {
    token = (static_cast<uint64_t>(rd()) << 32) ^ static_cast<uint64_t>(rd());
  } while (token == 0 || Registry().count(token) != 0);
  return token;
}

libwebrtc::VideoTrackImpl* AsVideoTrackImpl(lw_video_track_t* track) {
  if (!track) {
    return nullptr;
  }
  // The shim hands out handles wrapping a libwebrtc::RTCVideoTrack; validate
  // the concrete type before use.
  auto* iface = lw::From<libwebrtc::RTCVideoTrack>(track);
  return dynamic_cast<libwebrtc::VideoTrackImpl*>(iface);
}

}  // namespace

extern "C" {

lw_video_sink_token lw_video_sink_register(const LwVideoSinkV1* sink,
                                           void* user) {
  if (!sink || !sink->on_frame) {
    return 0;
  }
  webrtc::MutexLock lock(&RegistryMutex());
  lw_video_sink_token token = GenerateToken();
  Registry()[token] = SinkEntry{.sink = sink, .user = user};
  return token;
}

int lw_video_sink_unregister(lw_video_sink_token token) {
  webrtc::MutexLock lock(&RegistryMutex());
  return Registry().erase(token) != 0 ? 0 : -1;
}

lw_video_track_t* lw_video_track_find(const char* track_id) {
  if (track_id == nullptr || *track_id == '\0') {
    return nullptr;
  }
  libwebrtc::scoped_refptr<libwebrtc::RTCVideoTrack> track =
      libwebrtc::FindVideoTrackById(libwebrtc::string(track_id));
  if (!track.get()) {
    return nullptr;
  }
  // No producer: the factory behind this track belongs to whoever created it,
  // so this handle cannot promise to outlive it (see the header).
  return reinterpret_cast<lw_video_track_t*>(
      lw::Handle::Create(std::move(track), nullptr));
}

char* lw_video_track_id(lw_video_track_t* track) {
  auto* t = lw::From<libwebrtc::RTCVideoTrack>(track);
  return t != nullptr ? lw::DupString(t->id().c_string()) : nullptr;
}

int lw_video_track_bind_sink(lw_video_track_t* track,
                             lw_video_sink_token token) {
  libwebrtc::VideoTrackImpl* impl = AsVideoTrackImpl(track);
  if (!impl) {
    return -1;
  }

  SinkEntry entry;
  {
    webrtc::MutexLock lock(&RegistryMutex());
    auto it = Registry().find(token);
    if (it == Registry().end()) {
      return -1;
    }
    entry = it->second;
  }
  // Call into the adapter without holding the registry mutex to keep the two
  // locks independent.
  impl->SetNativeSink(entry.sink, entry.user);
  return 0;
}

int lw_video_track_unbind_sink(lw_video_track_t* track) {
  libwebrtc::VideoTrackImpl* impl = AsVideoTrackImpl(track);
  if (!impl) {
    return -1;
  }
  impl->SetNativeSink(nullptr, nullptr);  // quiesces, emits on_eos
  return 0;
}

int lw_video_track_get_stats(lw_video_track_t* track, LwVideoTrackStats* out) {
  libwebrtc::VideoTrackImpl* impl = AsVideoTrackImpl(track);
  // The caller states the size it compiled against, so a struct that grew
  // under it is rejected rather than written past.
  if (impl == nullptr || out == nullptr ||
      out->size != sizeof(LwVideoTrackStats)) {
    return -1;
  }
  impl->GetStats(out);
  return 0;
}

int lw_video_track_set_frame_callback(lw_video_track_t* track, lw_frame_cb cb,
                                      void* user) {
  libwebrtc::VideoTrackImpl* impl = AsVideoTrackImpl(track);
  if (!impl) {
    return -1;
  }
  impl->SetFrameObserver(cb, user);
  return 0;
}

}  // extern "C"
