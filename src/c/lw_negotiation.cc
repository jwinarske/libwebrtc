#include "c/lw_c_api.h"
#include "rtc_mediaconstraints.h"
#include "rtc_peerconnection.h"
#include "src/c/lw_handle.h"
#include "src/c/lw_string.h"

using libwebrtc::RTCMediaConstraints;
using libwebrtc::RTCPeerConnection;
using libwebrtc::scoped_refptr;
using libwebrtc::string;

extern "C" {

void lw_pc_create_offer(lw_pc_t* pc, lw_sdp_success_cb on_success,
                        lw_sdp_failure_cb on_failure, void* user) {
  auto* p = lw::From<RTCPeerConnection>(pc);
  if (!p) {
    if (on_failure) {
      on_failure(lw::DupString("null peer connection"), user);
    }
    return;
  }
  scoped_refptr<RTCMediaConstraints> constraints =
      RTCMediaConstraints::Create();
  p->CreateOffer(
      [on_success, user](const string& sdp, const string& type) {
        if (on_success) {
          on_success(lw::DupString(sdp.c_string()),
                     lw::DupString(type.c_string()), user);
        }
      },
      [on_failure, user](const char* error) {
        if (on_failure) {
          on_failure(lw::DupString(error), user);
        }
      },
      constraints);
}

void lw_pc_create_answer(lw_pc_t* pc, lw_sdp_success_cb on_success,
                         lw_sdp_failure_cb on_failure, void* user) {
  auto* p = lw::From<RTCPeerConnection>(pc);
  if (!p) {
    if (on_failure) {
      on_failure(lw::DupString("null peer connection"), user);
    }
    return;
  }
  scoped_refptr<RTCMediaConstraints> constraints =
      RTCMediaConstraints::Create();
  p->CreateAnswer(
      [on_success, user](const string& sdp, const string& type) {
        if (on_success) {
          on_success(lw::DupString(sdp.c_string()),
                     lw::DupString(type.c_string()), user);
        }
      },
      [on_failure, user](const char* error) {
        if (on_failure) {
          on_failure(lw::DupString(error), user);
        }
      },
      constraints);
}

void lw_pc_set_local_description(lw_pc_t* pc, const char* sdp, const char* type,
                                 lw_set_sdp_success_cb on_success,
                                 lw_sdp_failure_cb on_failure, void* user) {
  auto* p = lw::From<RTCPeerConnection>(pc);
  if (!p) {
    if (on_failure) {
      on_failure(lw::DupString("null peer connection"), user);
    }
    return;
  }
  p->SetLocalDescription(
      string(sdp ? sdp : ""), string(type ? type : ""),
      [on_success, user]() {
        if (on_success) {
          on_success(user);
        }
      },
      [on_failure, user](const char* error) {
        if (on_failure) {
          on_failure(lw::DupString(error), user);
        }
      });
}

void lw_pc_set_remote_description(lw_pc_t* pc, const char* sdp,
                                  const char* type,
                                  lw_set_sdp_success_cb on_success,
                                  lw_sdp_failure_cb on_failure, void* user) {
  auto* p = lw::From<RTCPeerConnection>(pc);
  if (!p) {
    if (on_failure) {
      on_failure(lw::DupString("null peer connection"), user);
    }
    return;
  }
  p->SetRemoteDescription(
      string(sdp ? sdp : ""), string(type ? type : ""),
      [on_success, user]() {
        if (on_success) {
          on_success(user);
        }
      },
      [on_failure, user](const char* error) {
        if (on_failure) {
          on_failure(lw::DupString(error), user);
        }
      });
}

void lw_pc_get_stats(lw_pc_t* pc, lw_stats_success_cb on_success,
                     lw_sdp_failure_cb on_failure, void* user) {
  auto* p = lw::From<RTCPeerConnection>(pc);
  if (!p) {
    if (on_failure) {
      on_failure(lw::DupString("null peer connection"), user);
    }
    return;
  }
  p->GetStats(
      [on_success,
       user](const libwebrtc::vector<scoped_refptr<libwebrtc::MediaRTCStats>>
                 reports) {
        if (!on_success) {
          return;
        }
        // Each report already serializes itself with its id, type and
        // timestamp, so the document is those objects in an array.
        std::string doc = "[";
        for (size_t i = 0; i < reports.size(); ++i) {
          if (i != 0) {
            doc += ',';
          }
          if (reports[i].get() != nullptr) {
            doc += reports[i]->ToJson().std_string();
          } else {
            doc += "null";
          }
        }
        doc += ']';
        on_success(lw::DupString(doc.c_str()), user);
      },
      [on_failure, user](const char* error) {
        if (on_failure) {
          on_failure(lw::DupString(error), user);
        }
      });
}

void lw_pc_add_ice_candidate(lw_pc_t* pc, const char* mid, int mline_index,
                             const char* candidate) {
  auto* p = lw::From<RTCPeerConnection>(pc);
  if (!p) {
    return;
  }
  p->AddCandidate(string(mid ? mid : ""), mline_index,
                  string(candidate ? candidate : ""));
}

}  // extern "C"
