#include <cstdint>
#include <map>
#include <memory>
#include <utility>

#include "c/lw_c_api.h"
#include "rtc_base/synchronization/mutex.h"
#include "rtc_data_channel.h"
#include "rtc_peerconnection.h"
#include "src/c/lw_handle.h"
#include "src/c/lw_string.h"

using libwebrtc::RTCDataChannel;
using libwebrtc::RTCDataChannelInit;
using libwebrtc::RTCDataChannelObserver;
using libwebrtc::RTCDataChannelState;
using libwebrtc::RTCPeerConnection;
using libwebrtc::scoped_refptr;
using libwebrtc::string;

namespace {

// The C enums are what a consumer sees; these keep them honest.
static_assert(LW_DATA_CHANNEL_CONNECTING ==
              static_cast<int>(libwebrtc::RTCDataChannelConnecting));
static_assert(LW_DATA_CHANNEL_OPEN ==
              static_cast<int>(libwebrtc::RTCDataChannelOpen));
static_assert(LW_DATA_CHANNEL_CLOSING ==
              static_cast<int>(libwebrtc::RTCDataChannelClosing));
static_assert(LW_DATA_CHANNEL_CLOSED ==
              static_cast<int>(libwebrtc::RTCDataChannelClosed));

// Adapts RTCDataChannelObserver to the flat callback table.
class CDataObserver : public RTCDataChannelObserver {
 public:
  CDataObserver(const LwDataChannelObserver& callbacks, void* user)
      : cb_(callbacks), user_(user) {}

  void OnStateChange(RTCDataChannelState state) override {
    if (cb_.on_state) {
      cb_.on_state(static_cast<int>(state), user_);
    }
  }

  void OnMessage(const char* buffer, int length, bool binary) override {
    if (cb_.on_message == nullptr || length < 0) {
      return;
    }
    const auto size = static_cast<uint32_t>(length);
    cb_.on_message(lw::DupBytes(buffer, size), size, binary ? 1 : 0, user_);
  }

 private:
  LwDataChannelObserver cb_;
  void* user_;
};

webrtc::Mutex& ObserverMutex() {
  static webrtc::Mutex* m = new webrtc::Mutex();
  return *m;
}

// Owns the observer adapters for as long as they are registered.
std::map<lw_data_channel_t*, std::unique_ptr<CDataObserver>>& Observers() {
  static std::map<lw_data_channel_t*, std::unique_ptr<CDataObserver>>* m =
      new std::map<lw_data_channel_t*, std::unique_ptr<CDataObserver>>();
  return *m;
}

}  // namespace

extern "C" {

lw_data_channel_t* lw_pc_create_data_channel(lw_pc_t* pc, const char* label,
                                             const LwDataChannelInit* init) {
  auto* p = lw::From<RTCPeerConnection>(pc);
  if (p == nullptr || label == nullptr || *label == '\0') {
    return nullptr;
  }
  RTCDataChannelInit config;
  if (init != nullptr) {
    if (init->size < sizeof(LwDataChannelInit)) {
      return nullptr;
    }
    config.ordered = init->ordered != 0;
    config.maxRetransmitTime = init->max_retransmit_time_ms;
    config.maxRetransmits = init->max_retransmits;
    config.negotiated = init->negotiated != 0;
    config.id = init->id;
    // reliable is implied by leaving both retransmit limits unset.
    config.reliable =
        init->max_retransmit_time_ms < 0 && init->max_retransmits < 0;
  }
  scoped_refptr<RTCDataChannel> channel = p->CreateDataChannel(label, &config);
  return reinterpret_cast<lw_data_channel_t*>(
      lw::Derive(pc, std::move(channel)));
}

int lw_data_channel_send(lw_data_channel_t* channel, const uint8_t* data,
                         uint32_t size, int binary) {
  auto* c = lw::From<RTCDataChannel>(channel);
  if (c == nullptr || (data == nullptr && size != 0)) {
    return -1;
  }
  c->Send(data, size, binary != 0);
  return 0;
}

void lw_data_channel_close(lw_data_channel_t* channel) {
  if (auto* c = lw::From<RTCDataChannel>(channel)) {
    c->Close();
  }
}

int lw_data_channel_id(lw_data_channel_t* channel) {
  auto* c = lw::From<RTCDataChannel>(channel);
  return c != nullptr ? c->id() : -1;
}

int lw_data_channel_get_state(lw_data_channel_t* channel) {
  auto* c = lw::From<RTCDataChannel>(channel);
  return c != nullptr ? static_cast<int>(c->state()) : -1;
}

uint64_t lw_data_channel_buffered_amount(lw_data_channel_t* channel) {
  auto* c = lw::From<RTCDataChannel>(channel);
  return c != nullptr ? c->buffered_amount() : 0;
}

char* lw_data_channel_label(lw_data_channel_t* channel) {
  auto* c = lw::From<RTCDataChannel>(channel);
  return c != nullptr ? lw::DupString(c->label().c_string()) : nullptr;
}

int lw_data_channel_set_observer(lw_data_channel_t* channel,
                                 const LwDataChannelObserver* observer,
                                 void* user) {
  auto* c = lw::From<RTCDataChannel>(channel);
  if (c == nullptr || observer == nullptr ||
      observer->size < sizeof(LwDataChannelObserver)) {
    return -1;
  }
  auto adapter = std::make_unique<CDataObserver>(*observer, user);

  webrtc::MutexLock lock(&ObserverMutex());
  auto it = Observers().find(channel);
  if (it != Observers().end()) {
    c->UnregisterObserver();
    it->second = std::move(adapter);
  } else {
    Observers().emplace(channel, std::move(adapter));
    it = Observers().find(channel);
  }
  c->RegisterObserver(it->second.get());
  return 0;
}

void lw_data_channel_remove_observer(lw_data_channel_t* channel) {
  auto* c = lw::From<RTCDataChannel>(channel);
  if (c == nullptr) {
    return;
  }
  webrtc::MutexLock lock(&ObserverMutex());
  auto it = Observers().find(channel);
  if (it != Observers().end()) {
    c->UnregisterObserver();
    Observers().erase(it);
  }
}

}  // extern "C"
