#include "rtc_video_track_impl.h"

#include <map>
#include <string>

#include "rtc_base/logging.h"
#include "rtc_base/synchronization/mutex.h"
#include "rtc_media_stream_impl.h"

namespace libwebrtc {
namespace {

// Live video tracks by id. Raw pointers: a track removes itself on the way
// out, and a lookup retains under the same lock, so a caller never receives a
// pointer that is already being destroyed.
webrtc::Mutex& TrackRegistryMutex() {
  static webrtc::Mutex* m = new webrtc::Mutex();
  return *m;
}

std::map<std::string, VideoTrackImpl*>& TrackRegistry() {
  static std::map<std::string, VideoTrackImpl*>* m =
      new std::map<std::string, VideoTrackImpl*>();
  return *m;
}

}  // namespace

scoped_refptr<RTCVideoTrack> FindVideoTrackById(const string& id) {
  webrtc::MutexLock lock(&TrackRegistryMutex());
  const auto it = TrackRegistry().find(id.std_string());
  if (it == TrackRegistry().end()) {
    return nullptr;
  }
  // Retained here, under the lock the destructor also takes, so the track
  // cannot reach its destructor between this lookup and the caller's use.
  return scoped_refptr<RTCVideoTrack>(it->second);
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
  TrackRegistry()[id_.std_string()] = this;
}

VideoTrackImpl::~VideoTrackImpl() {
  RTC_LOG(LS_INFO) << __FUNCTION__ << ": dtor ";
  webrtc::MutexLock lock(&TrackRegistryMutex());
  const auto it = TrackRegistry().find(id_.std_string());
  // Only if it is still ours: a same-id track constructed later replaced this
  // entry, and removing it then would unpublish the live one.
  if (it != TrackRegistry().end() && it->second == this) {
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
