#include <cstring>
#include <map>
#include <memory>
#include <utility>

#include "c/lw_c_api.h"
#include "rtc_base/synchronization/mutex.h"
#include "rtc_data_channel.h"
#include "rtc_ice_candidate.h"
#include "rtc_peerconnection.h"
#include "rtc_rtp_transceiver.h"
#include "src/c/lw_handle.h"
#include "src/c/lw_string.h"

using libwebrtc::RefCountInterface;
using libwebrtc::RTCIceCandidate;
using libwebrtc::RTCPeerConnection;
using libwebrtc::RTCPeerConnectionObserver;
using libwebrtc::RTCRtpTransceiver;
using libwebrtc::scoped_refptr;

namespace {

// The C enums are what a consumer sees; these keep them honest against the
// values actually delivered.
static_assert(LW_SIGNALING_STABLE ==
              static_cast<int>(libwebrtc::RTCSignalingStateStable));
static_assert(LW_SIGNALING_HAVE_LOCAL_OFFER ==
              static_cast<int>(libwebrtc::RTCSignalingStateHaveLocalOffer));
static_assert(LW_SIGNALING_HAVE_REMOTE_OFFER ==
              static_cast<int>(libwebrtc::RTCSignalingStateHaveRemoteOffer));
static_assert(LW_SIGNALING_HAVE_LOCAL_PRANSWER ==
              static_cast<int>(libwebrtc::RTCSignalingStateHaveLocalPrAnswer));
static_assert(LW_SIGNALING_HAVE_REMOTE_PRANSWER ==
              static_cast<int>(libwebrtc::RTCSignalingStateHaveRemotePrAnswer));
static_assert(LW_SIGNALING_CLOSED ==
              static_cast<int>(libwebrtc::RTCSignalingStateClosed));

static_assert(LW_PC_STATE_NEW ==
              static_cast<int>(libwebrtc::RTCPeerConnectionStateNew));
static_assert(LW_PC_STATE_CONNECTING ==
              static_cast<int>(libwebrtc::RTCPeerConnectionStateConnecting));
static_assert(LW_PC_STATE_CONNECTED ==
              static_cast<int>(libwebrtc::RTCPeerConnectionStateConnected));
static_assert(LW_PC_STATE_DISCONNECTED ==
              static_cast<int>(libwebrtc::RTCPeerConnectionStateDisconnected));
static_assert(LW_PC_STATE_FAILED ==
              static_cast<int>(libwebrtc::RTCPeerConnectionStateFailed));
static_assert(LW_PC_STATE_CLOSED ==
              static_cast<int>(libwebrtc::RTCPeerConnectionStateClosed));

static_assert(LW_ICE_GATHERING_NEW ==
              static_cast<int>(libwebrtc::RTCIceGatheringStateNew));
static_assert(LW_ICE_GATHERING_GATHERING ==
              static_cast<int>(libwebrtc::RTCIceGatheringStateGathering));
static_assert(LW_ICE_GATHERING_COMPLETE ==
              static_cast<int>(libwebrtc::RTCIceGatheringStateComplete));

static_assert(LW_ICE_CONNECTION_NEW ==
              static_cast<int>(libwebrtc::RTCIceConnectionStateNew));
static_assert(LW_ICE_CONNECTION_CHECKING ==
              static_cast<int>(libwebrtc::RTCIceConnectionStateChecking));
static_assert(LW_ICE_CONNECTION_COMPLETED ==
              static_cast<int>(libwebrtc::RTCIceConnectionStateCompleted));
static_assert(LW_ICE_CONNECTION_CONNECTED ==
              static_cast<int>(libwebrtc::RTCIceConnectionStateConnected));
static_assert(LW_ICE_CONNECTION_FAILED ==
              static_cast<int>(libwebrtc::RTCIceConnectionStateFailed));
static_assert(LW_ICE_CONNECTION_DISCONNECTED ==
              static_cast<int>(libwebrtc::RTCIceConnectionStateDisconnected));
static_assert(LW_ICE_CONNECTION_CLOSED ==
              static_cast<int>(libwebrtc::RTCIceConnectionStateClosed));

// Adapts the RTCPeerConnectionObserver C++ interface to the flat LwPcObserver
// callback table. All virtuals are overridden; those without a callback are
// no-ops.
class CObserver : public RTCPeerConnectionObserver {
 public:
  CObserver(const LwPcObserver& callbacks, void* user,
            scoped_refptr<RefCountInterface> producer)
      : cb_(callbacks), user_(user), producer_(std::move(producer)) {}

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
      cb_.on_ice_candidate(lw::DupString(candidate->candidate().c_string()),
                           lw::DupString(candidate->sdp_mid().c_string()),
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
      // Hand the callback an owning handle (retired with lw_release), tied to
      // the same factory as the peer connection this observer is on.
      cb_.on_track(reinterpret_cast<lw_transceiver_t*>(
                       lw::Handle::Create(std::move(transceiver), producer_)),
                   user_);
    }
  }

  void OnDataChannel(
      scoped_refptr<libwebrtc::RTCDataChannel> channel) override {
    if (cb_.on_data_channel && channel.get()) {
      // Hand the callback an owning handle, tied to the same factory as the
      // peer connection this observer is on.
      cb_.on_data_channel(
          reinterpret_cast<lw_data_channel_t*>(
              lw::Handle::Create(std::move(channel), producer_)),
          user_);
    }
  }

  // Unused events: no-op overrides to satisfy the interface.
  void OnAddStream(scoped_refptr<libwebrtc::RTCMediaStream>) override {}
  void OnRemoveStream(scoped_refptr<libwebrtc::RTCMediaStream>) override {}
  void OnAddTrack(libwebrtc::vector<scoped_refptr<libwebrtc::RTCMediaStream>>,
                  scoped_refptr<libwebrtc::RTCRtpReceiver>) override {}
  void OnRemoveTrack(scoped_refptr<libwebrtc::RTCRtpReceiver>) override {}

 private:
  LwPcObserver cb_;
  void* user_;
  const scoped_refptr<RefCountInterface> producer_;
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
  auto* p = lw::From<RTCPeerConnection>(pc);
  if (!p) {
    return -1;
  }
  auto adapter = std::make_unique<CObserver>(
      *observer, user, reinterpret_cast<lw::Handle*>(pc)->producer());

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
  auto* p = lw::From<RTCPeerConnection>(pc);
  if (!p) {
    return;
  }
  webrtc::MutexLock lock(&ObserverMutex());
  auto it = Observers().find(pc);
  if (it != Observers().end()) {
    p->DeRegisterRTCPeerConnectionObserver();
    Observers().erase(it);
  }
}

}  // extern "C"
