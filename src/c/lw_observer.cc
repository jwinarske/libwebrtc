#include <cstring>
#include <map>
#include <memory>

#include "c/lw_c_api.h"
#include "rtc_base/synchronization/mutex.h"
#include "rtc_ice_candidate.h"
#include "rtc_peerconnection.h"
#include "rtc_rtp_transceiver.h"

using libwebrtc::RTCIceCandidate;
using libwebrtc::RTCPeerConnection;
using libwebrtc::RTCPeerConnectionObserver;
using libwebrtc::RTCRtpTransceiver;
using libwebrtc::scoped_refptr;

namespace {

// Adapts the RTCPeerConnectionObserver C++ interface to the flat LwPcObserver
// callback table. All virtuals are overridden; those without a callback are
// no-ops.
class CObserver : public RTCPeerConnectionObserver {
 public:
  CObserver(const LwPcObserver& callbacks, void* user)
      : cb_(callbacks), user_(user) {}

  void OnSignalingState(libwebrtc::RTCSignalingState state) override {
    if (cb_.on_signaling_state) {
      cb_.on_signaling_state(static_cast<int>(state), user_);
    }
  }
  void OnPeerConnectionState(libwebrtc::RTCPeerConnectionState state) override {
    if (cb_.on_connection_state) {
      cb_.on_connection_state(static_cast<int>(state), user_);
    }
  }
  void OnIceGatheringState(libwebrtc::RTCIceGatheringState state) override {
    if (cb_.on_ice_gathering_state) {
      cb_.on_ice_gathering_state(static_cast<int>(state), user_);
    }
  }
  void OnIceConnectionState(libwebrtc::RTCIceConnectionState state) override {
    if (cb_.on_ice_connection_state) {
      cb_.on_ice_connection_state(static_cast<int>(state), user_);
    }
  }
  void OnIceCandidate(scoped_refptr<RTCIceCandidate> candidate) override {
    if (cb_.on_ice_candidate && candidate.get()) {
      cb_.on_ice_candidate(candidate->candidate().c_string(),
                           candidate->sdp_mid().c_string(),
                           candidate->sdp_mline_index(), user_);
    }
  }
  void OnRenegotiationNeeded() override {
    if (cb_.on_renegotiation_needed) {
      cb_.on_renegotiation_needed(user_);
    }
  }
  void OnTrack(scoped_refptr<RTCRtpTransceiver> transceiver) override {
    if (cb_.on_track && transceiver.get()) {
      // Hand the callback an owning handle (retired with lw_release).
      transceiver->AddRef();
      cb_.on_track(reinterpret_cast<lw_transceiver_t*>(transceiver.get()),
                   user_);
    }
  }

  // Unused events: no-op overrides to satisfy the interface.
  void OnAddStream(scoped_refptr<libwebrtc::RTCMediaStream>) override {}
  void OnRemoveStream(scoped_refptr<libwebrtc::RTCMediaStream>) override {}
  void OnDataChannel(scoped_refptr<libwebrtc::RTCDataChannel>) override {}
  void OnAddTrack(libwebrtc::vector<scoped_refptr<libwebrtc::RTCMediaStream>>,
                  scoped_refptr<libwebrtc::RTCRtpReceiver>) override {}
  void OnRemoveTrack(scoped_refptr<libwebrtc::RTCRtpReceiver>) override {}

 private:
  LwPcObserver cb_;
  void* user_;
};

webrtc::Mutex& ObserverMutex() {
  static webrtc::Mutex* m = new webrtc::Mutex();
  return *m;
}

// Owns the observer adapters for as long as they are registered.
std::map<lw_pc_t*, std::unique_ptr<CObserver>>& Observers() {
  static std::map<lw_pc_t*, std::unique_ptr<CObserver>>* m =
      new std::map<lw_pc_t*, std::unique_ptr<CObserver>>();
  return *m;
}

}  // namespace

extern "C" {

int lw_pc_set_observer(lw_pc_t* pc, const LwPcObserver* observer, void* user) {
  if (!pc || !observer || observer->size < sizeof(LwPcObserver)) {
    return -1;
  }
  auto* p = reinterpret_cast<RTCPeerConnection*>(pc);
  auto adapter = std::make_unique<CObserver>(*observer, user);

  webrtc::MutexLock lock(&ObserverMutex());
  auto it = Observers().find(pc);
  if (it != Observers().end()) {
    p->DeRegisterRTCPeerConnectionObserver();
    it->second = std::move(adapter);
  } else {
    Observers().emplace(pc, std::move(adapter));
    it = Observers().find(pc);
  }
  p->RegisterRTCPeerConnectionObserver(it->second.get());
  return 0;
}

void lw_pc_remove_observer(lw_pc_t* pc) {
  if (!pc) {
    return;
  }
  auto* p = reinterpret_cast<RTCPeerConnection*>(pc);
  webrtc::MutexLock lock(&ObserverMutex());
  auto it = Observers().find(pc);
  if (it != Observers().end()) {
    p->DeRegisterRTCPeerConnectionObserver();
    Observers().erase(it);
  }
}

}  // extern "C"
