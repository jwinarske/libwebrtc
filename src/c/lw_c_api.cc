#include "c/lw_c_api.h"

#include <cstdlib>
#include <cstring>

#include "base/refcount.h"
#include "libwebrtc.h"
#include "rtc_base/logging.h"
#include "rtc_peerconnection_factory.h"

using libwebrtc::LibWebRTC;
using libwebrtc::RefCountInterface;
using libwebrtc::RTCPeerConnectionFactory;

extern "C" {

int lw_abi_version(void) { return LW_ABI_VERSION; }

int lw_initialize(void) {
  // Opt-in diagnostic logging to stderr: LW_LOG=verbose|info (any other
  // non-empty value maps to info). Off by default.
  if (const char* lvl = std::getenv("LW_LOG")) {
    webrtc::LogMessage::SetLogToStderr(true);
    webrtc::LogMessage::LogToDebug(std::strcmp(lvl, "verbose") == 0
                                       ? webrtc::LS_VERBOSE
                                       : webrtc::LS_INFO);
  }
  return LibWebRTC::Initialize() ? 1 : 0;
}

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
