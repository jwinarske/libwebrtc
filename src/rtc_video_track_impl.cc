#include "rtc_video_track_impl.h"

#include <map>
#include <string>

#include "rtc_base/logging.h"
#include "rtc_base/synchronization/mutex.h"
#include "rtc_media_stream_impl.h"

namespace libwebrtc {
namespace {

// Live video tracks by id.
//
// The entry holds the *underlying* webrtc track, not the wrapper that
// published it. Retaining a wrapper here would be unsafe: a wrapper enters its
// destructor once its count reaches zero and only then takes this lock, so a
// lookup that won the lock first would resurrect a dying object. Holding the
// underlying track instead means a lookup retains something that is alive by
// construction, and builds a fresh wrapper around it.
webrtc::Mutex& TrackRegistryMutex() {
  static webrtc::Mutex* m = new webrtc::Mutex();
  return *m;
}

struct Published {
  webrtc::scoped_refptr<webrtc::VideoTrackInterface> track;
  // How many live wrappers carry this id. An id stays findable while any of
  // them is alive: a lookup builds a wrapper of its own, and disposing that
  // one must not unpublish an id the original still holds.
  int wrappers;
};

std::map<std::string, Published>& TrackRegistry() {
  static std::map<std::string, Published>* m =
      new std::map<std::string, Published>();
  return *m;
}

}  // namespace

scoped_refptr<RTCVideoTrack> FindVideoTrackById(const string& id) {
  webrtc::scoped_refptr<webrtc::VideoTrackInterface> underlying;
  {
    webrtc::MutexLock lock(&TrackRegistryMutex());
    const auto it = TrackRegistry().find(id.std_string());
    if (it == TrackRegistry().end()) {
      return nullptr;
    }
    underlying = it->second.track;  // alive: the registry holds a reference
  }
  // A fresh wrapper, with its own sink binding and its own adapter on the
  // underlying track -- the same shape as any other wrapper of it. Built
  // outside the lock, since constructing one publishes and would re-enter.
  return scoped_refptr<RTCVideoTrack>(
      new RefCountedObject<VideoTrackImpl>(underlying));
}

VideoTrackImpl::VideoTrackImpl(
    webrtc::scoped_refptr<webrtc::VideoTrackInterface> rtc_track)
    : rtc_track_(rtc_track),
      video_sink_(new RefCountedObject<VideoSinkAdapter>(rtc_track)) {
  RTC_LOG(LS_INFO) << __FUNCTION__ << ": ctor ";
  id_ = rtc_track_->id();
  kind_ = rtc_track_->kind();

  // Published so a consumer that did not create this track can find it by id.
  // A duplicate id replaces the older entry: webrtc ids are unique among live
  // tracks, and the newer one is the reachable track.
  webrtc::MutexLock lock(&TrackRegistryMutex());
  auto& entry = TrackRegistry()[id_.std_string()];
  entry.track = rtc_track_;  // newest wins, as the id may not be unique
  ++entry.wrappers;
}

VideoTrackImpl::~VideoTrackImpl() {
  RTC_LOG(LS_INFO) << __FUNCTION__ << ": dtor ";
  webrtc::MutexLock lock(&TrackRegistryMutex());
  const auto it = TrackRegistry().find(id_.std_string());
  // Unpublished only when the last wrapper carrying this id goes.
  if (it != TrackRegistry().end() && --it->second.wrappers <= 0) {
    TrackRegistry().erase(it);
  }
}

void VideoTrackImpl::AddRenderer(
    RTCVideoRenderer<scoped_refptr<RTCVideoFrame>>* renderer) {
  return video_sink_->AddRenderer(renderer);
}

void VideoTrackImpl::RemoveRenderer(
    RTCVideoRenderer<scoped_refptr<RTCVideoFrame>>* renderer) {
  return video_sink_->RemoveRenderer(renderer);
}

void VideoTrackImpl::SetNativeSink(const LwVideoSinkV1* sink, void* user) {
  video_sink_->SetNativeSink(sink, user);
}

void VideoTrackImpl::SetFrameObserver(void (*cb)(int, int, void*), void* user) {
  video_sink_->SetFrameObserver(cb, user);
}

void VideoTrackImpl::GetStats(LwVideoTrackStats* out) const {
  video_sink_->GetStats(out);
}

}  // namespace libwebrtc
