#include "c/lw_c_api.h"

#include "base/refcount.h"
#include "libwebrtc.h"
#include "rtc_peerconnection_factory.h"

using libwebrtc::LibWebRTC;
using libwebrtc::RefCountInterface;
using libwebrtc::RTCPeerConnectionFactory;

extern "C" {

int lw_abi_version(void) { return LW_ABI_VERSION; }

int lw_initialize(void) { return LibWebRTC::Initialize() ? 1 : 0; }

void lw_terminate(void) { LibWebRTC::Terminate(); }

lw_factory_t* lw_factory_create(void) {
  libwebrtc::scoped_refptr<RTCPeerConnectionFactory> factory =
      LibWebRTC::CreateRTCPeerConnectionFactory();
  if (!factory.get()) {
    return nullptr;
  }
  // Transfer the reference into the opaque handle (retired via lw_release).
  // RTCPeerConnectionFactory derives from RefCountInterface as its first base,
  // so the pointer value is valid for both this handle and lw_retain/release.
  return reinterpret_cast<lw_factory_t*>(factory.release());
}

int lw_factory_initialize(lw_factory_t* factory) {
  if (!factory) {
    return 0;
  }
  return reinterpret_cast<RTCPeerConnectionFactory*>(factory)->Initialize() ? 1
                                                                            : 0;
}

void lw_retain(void* handle) {
  if (handle) {
    reinterpret_cast<RefCountInterface*>(handle)->AddRef();
  }
}

void lw_release(void* handle) {
  if (handle) {
    reinterpret_cast<RefCountInterface*>(handle)->Release();
  }
}

}  // extern "C"
