#include "c/lw_c_api.h"

#include <cstdlib>
#include <cstring>
#include <utility>

#include "base/refcount.h"
#include "libwebrtc.h"
#include "rtc_base/logging.h"
#include "rtc_peerconnection_factory.h"
#include "src/c/lw_handle.h"
#include "src/c/lw_string.h"

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
  // The factory is its own producer: handles derived from it take a reference
  // to it, so it outlives every peer connection, transceiver and track the
  // caller obtained through it regardless of release order (see lw_handle.h).
  return reinterpret_cast<lw_factory_t*>(
      lw::Handle::Create(std::move(factory), nullptr));
}

int lw_factory_initialize(lw_factory_t* factory) {
  auto* f = lw::From<RTCPeerConnectionFactory>(factory);
  if (!f) {
    return 0;
  }
  return f->Initialize() ? 1 : 0;
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

void lw_string_free(char* s) { std::free(s); }

}  // extern "C"
