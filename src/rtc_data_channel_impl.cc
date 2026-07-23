#include "rtc_data_channel_impl.h"

namespace libwebrtc {
namespace {

RTCDataChannelState ToRTCDataChannelState(
    webrtc::DataChannelInterface::DataState state) {
  switch (state) {
    case webrtc::DataChannelInterface::kConnecting:
      return RTCDataChannelConnecting;
    case webrtc::DataChannelInterface::kOpen:
      return RTCDataChannelOpen;
    case webrtc::DataChannelInterface::kClosing:
      return RTCDataChannelClosing;
    case webrtc::DataChannelInterface::kClosed:
      return RTCDataChannelClosed;
  }
  return RTCDataChannelClosed;
}

}  // namespace

RTCDataChannelImpl::RTCDataChannelImpl(
    webrtc::scoped_refptr<webrtc::DataChannelInterface> rtc_data_channel)
    : rtc_data_channel_(rtc_data_channel), crit_sect_(new webrtc::Mutex()) {
  // Seed from the channel rather than waiting for the first state change:
  // until one arrives, state() would otherwise report whatever was in the
  // member.
  state_ = ToRTCDataChannelState(rtc_data_channel_->state());
  rtc_data_channel_->RegisterObserver(this);
  label_ = rtc_data_channel_->label();
}

RTCDataChannelImpl::~RTCDataChannelImpl() {
  rtc_data_channel_->UnregisterObserver();
}

void RTCDataChannelImpl::Send(const uint8_t* data, uint32_t size,
                              bool binary /*= false*/) {
  webrtc::CopyOnWriteBuffer copyOnWriteBuffer(data, size);
  webrtc::DataBuffer buffer(copyOnWriteBuffer, binary);
  rtc_data_channel_->Send(buffer);
}

void RTCDataChannelImpl::Close() {
  // The observer stays registered: closing is a state change like any other,
  // and dropping the observer first is what made state() report `connecting`
  // for the rest of the channel's life. The destructor unregisters.
  rtc_data_channel_->Close();
}

void RTCDataChannelImpl::RegisterObserver(RTCDataChannelObserver* observer) {
  webrtc::MutexLock lock(crit_sect_.get());
  observer_ = observer;
}

void RTCDataChannelImpl::UnregisterObserver() {
  webrtc::MutexLock lock(crit_sect_.get());
  observer_ = nullptr;
}

const string RTCDataChannelImpl::label() const { return label_; }

int RTCDataChannelImpl::id() const { return rtc_data_channel_->id(); }

uint64_t RTCDataChannelImpl::buffered_amount() const {
  return rtc_data_channel_->buffered_amount();
}

void RTCDataChannelImpl::OnStateChange() {
  const RTCDataChannelState state =
      ToRTCDataChannelState(rtc_data_channel_->state());
  RTCDataChannelObserver* observer = nullptr;
  {
    webrtc::MutexLock lock(crit_sect_.get());
    state_ = state;
    observer = observer_;
  }
  // Called without the lock: an observer is free to ask this channel for its
  // state, which would deadlock on a non-recursive mutex.
  if (observer) {
    observer->OnStateChange(state);
  }
}

RTCDataChannelState RTCDataChannelImpl::state() {
  webrtc::MutexLock lock(crit_sect_.get());
  return state_;
}

void RTCDataChannelImpl::OnMessage(const webrtc::DataBuffer& buffer) {
  RTCDataChannelObserver* observer = nullptr;
  {
    webrtc::MutexLock lock(crit_sect_.get());
    observer = observer_;
  }
  if (observer) {
    observer->OnMessage(buffer.data.data<char>(),
                        static_cast<int>(buffer.data.size()), buffer.binary);
  }
}

}  // namespace libwebrtc
