// SPDX-FileCopyrightText: 2026 Joel Winarske
// SPDX-License-Identifier: MIT

// End-to-end check of the zero-copy video path, exercised over a real
// PeerConnection rather than a stub. A raw I420 frame is fed to a local video
// source, encoded as H.264, sent across a loopback PeerConnection, decoded by
// the absorbed hardware decoder, and delivered to a native sink registered
// through the flat C API. Passing means a decoded frame reached the sink as a
// dma-buf descriptor without ever being copied through the CPU renderer path.
//
// It covers the joins that unit tests cannot: codec negotiation, the decoder
// emitting a native (kNative) buffer, the sink adapter recognising it, and the
// on_format / on_frame / release contract holding under real traffic.
//
// Requires a working H.264 decode device (a V4L2 M2M decoder, or a VAAPI
// device such as /dev/dri/renderD128) and a library built with
// lw_enable_v4l2_codec=true. It is not part of the unit test suite because it
// depends on the host having such a device. Run it directly:
//
//   ninja -C out/<dir> lw_native_frame_e2e
//   cd out/<dir> && LD_LIBRARY_PATH=. ./lw_native_frame_e2e
//
// Set LW_E2E_VERBOSE=1 to echo the library's decoder logging. Exits 0 on pass.

#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "c/lw_c_api.h"
#include "libwebrtc.h"
#include "rtc_logging.h"
#include "rtc_peerconnection.h"
#include "rtc_peerconnection_factory.h"
#include "rtc_video_frame.h"
#include "rtc_video_source.h"
#include "rtc_video_track.h"

namespace {

using namespace libwebrtc;

constexpr int kWidth = 320;
constexpr int kHeight = 240;
constexpr int kWantFrames = 10;

std::atomic<int> g_frames{0};
std::atomic<int> g_formats{0};
std::atomic<int> g_bad{0};
std::atomic<int> g_delivered{0};
std::atomic<int> g_frames_before_format{0};
std::atomic<bool> g_connected{false};

// ---- the native sink: the far end of the whole pipeline --------------------

void OnFormat(const LwDmabufDescriptor* d, void*) {
  ++g_formats;
  std::printf(
      "  sink on_format %ux%u fourcc=%.4s modifier=0x%llx generation=%u\n",
      d->width, d->height, reinterpret_cast<const char*>(&d->fourcc),
      static_cast<unsigned long long>(d->modifier), d->pool_generation);
}

int OnFrame(const LwDmabufDescriptor* d, LwFrameRelease release, void* ctx,
            void*) {
  // on_format must precede the first frame of a generation.
  if (g_formats == 0) {
    ++g_frames_before_format;
  }
  struct stat st;
  const bool fd_ok = d->planes[0].fd >= 0 && fstat(d->planes[0].fd, &st) == 0;
  if (!fd_ok || d->width != kWidth || d->height != kHeight ||
      d->num_planes == 0 || d->num_planes > LW_MAX_PLANES ||
      d->size < sizeof(LwDmabufDescriptor)) {
    ++g_bad;
  }
  if (g_frames < 3) {
    std::printf(
        "  sink on_frame  %ux%u fourcc=%.4s planes=%u fd=%d(%s) pitch=%u "
        "seq=%llu\n",
        d->width, d->height, reinterpret_cast<const char*>(&d->fourcc),
        d->num_planes, d->planes[0].fd, fd_ok ? "valid dma-buf" : "INVALID",
        d->planes[0].pitch, static_cast<unsigned long long>(d->frame_seq));
  }
  ++g_frames;
  if (release != nullptr) {
    release(ctx);  // take it, then return the buffer to the decoder pool
  }
  return 1;  // taken
}

void OnEos(void*) {}

// Counts every frame the track delivers, native or not, before the split.
// Anything counted here but not by the sink took a software path.
void OnAnyFrame(int /*width*/, int /*height*/, void*) { ++g_delivered; }

// ---- keep only H.264 in the offer, so the hardware decoder is exercised ----

std::string ForceH264(const std::string& sdp) {
  std::istringstream in(sdp);
  std::vector<std::string> lines;
  std::vector<std::string> keep;
  for (std::string line; std::getline(in, line);) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    lines.push_back(line);
  }
  for (const std::string& line : lines) {
    if (line.rfind("a=rtpmap:", 0) == 0 &&
        line.find(" H264/") != std::string::npos) {
      keep.push_back(line.substr(9, line.find(' ') - 9));
    }
  }
  if (keep.empty()) {
    return sdp;
  }
  std::string out;
  bool in_video = false;
  for (const std::string& line : lines) {
    if (line.rfind("m=", 0) == 0) {
      in_video = (line.rfind("m=video", 0) == 0);
    }
    if (in_video && line.rfind("m=video", 0) == 0) {
      std::istringstream ms(line);
      std::string tok;
      std::string hdr;
      for (int i = 0; i < 3 && (ms >> tok); ++i) {
        hdr += tok + " ";
      }
      out += hdr;
      for (size_t i = 0; i < keep.size(); ++i) {
        out += keep[i] + (i + 1 < keep.size() ? " " : "");
      }
      out += "\r\n";
      continue;
    }
    if (in_video &&
        (line.rfind("a=rtpmap:", 0) == 0 || line.rfind("a=fmtp:", 0) == 0 ||
         line.rfind("a=rtcp-fb:", 0) == 0)) {
      const size_t colon = line.find(':') + 1;
      const std::string pt = line.substr(colon, line.find(' ', colon) - colon);
      bool kept = false;
      for (const std::string& k : keep) {
        kept = kept || (k == pt);
      }
      if (!kept) {
        continue;
      }
    }
    out += line + "\r\n";
  }
  return out;
}

struct Candidate {
  std::string mid;
  int index = 0;
  std::string candidate;
};

// The sending peer, driven through the C++ interface: creating a video source
// and attaching a track is not part of the C ABI yet.
class Peer : public RTCPeerConnectionObserver {
 public:
  const char* name = "";
  scoped_refptr<RTCPeerConnection> pc;
  // Where local candidates go, and whether the far side can accept them yet.
  std::function<void(const Candidate&)> deliver;
  std::atomic<bool>* peer_ready = nullptr;
  std::atomic<bool> remote_set{false};

  void OnSignalingState(RTCSignalingState) override {}
  void OnPeerConnectionState(RTCPeerConnectionState state) override {
    if (state == RTCPeerConnectionStateConnected) {
      g_connected = true;
    }
  }
  void OnIceGatheringState(RTCIceGatheringState) override {}
  void OnIceConnectionState(RTCIceConnectionState) override {}

  void OnIceCandidate(scoped_refptr<RTCIceCandidate> c) override {
    if (!c || !deliver) {
      return;
    }
    Candidate cand{.mid = c->sdp_mid().std_string(),
                   .index = c->sdp_mline_index(),
                   .candidate = c->candidate().std_string()};
    // A candidate is only accepted once the far side has a remote
    // description; buffer until then or it is silently dropped.
    if (peer_ready != nullptr && peer_ready->load()) {
      deliver(cand);
    } else {
      std::lock_guard<std::mutex> lock(mu_);
      outbox_.push_back(std::move(cand));
    }
  }

  void FlushCandidates() {
    std::lock_guard<std::mutex> lock(mu_);
    for (const Candidate& c : outbox_) {
      deliver(c);
    }
    outbox_.clear();
  }

  void OnAddStream(scoped_refptr<RTCMediaStream>) override {}
  void OnRemoveStream(scoped_refptr<RTCMediaStream>) override {}
  void OnDataChannel(scoped_refptr<RTCDataChannel>) override {}
  void OnRenegotiationNeeded() override {}

  void OnTrack(scoped_refptr<RTCRtpTransceiver>) override {}

  void OnAddTrack(vector<scoped_refptr<RTCMediaStream>>,
                  scoped_refptr<RTCRtpReceiver>) override {}
  void OnRemoveTrack(scoped_refptr<RTCRtpReceiver>) override {}

 private:
  std::mutex mu_;
  std::vector<Candidate> outbox_;
};

// The receiving peer, driven entirely through the C ABI: this is the surface a
// dart:ffi or other foreign consumer sees, so the test exercises it rather than
// the C++ interface underneath.
struct Receiver {
  lw_pc_t* pc = nullptr;
  lw_video_track_t* track = nullptr;
  lw_video_sink_token token = 0;
  std::atomic<bool> remote_set{false};
  Peer* sender = nullptr;

  void Deliver(const Candidate& c) {
    lw_pc_add_ice_candidate(pc, c.mid.c_str(), c.index, c.candidate.c_str());
  }

  void FlushCandidates() {
    std::lock_guard<std::mutex> lock(mu);
    for (const Candidate& c : outbox) {
      sender->pc->AddCandidate(c.mid.c_str(), c.index, c.candidate.c_str());
    }
    outbox.clear();
  }

  std::mutex mu;
  std::vector<Candidate> outbox;
};

void RxOnConnectionState(int state, void*) {
  if (state == RTCPeerConnectionStateConnected) {
    g_connected = true;
  }
}

void RxOnIceCandidate(const char* candidate, const char* mid, int mline_index,
                      void* user) {
  auto* rx = static_cast<Receiver*>(user);
  // The strings are borrowed for the duration of the call, so copy now.
  Candidate cand{.mid = mid != nullptr ? mid : "",
                 .index = mline_index,
                 .candidate = candidate != nullptr ? candidate : ""};
  if (rx->sender->remote_set) {
    rx->sender->pc->AddCandidate(cand.mid.c_str(), cand.index,
                                 cand.candidate.c_str());
  } else {
    std::lock_guard<std::mutex> lock(rx->mu);
    rx->outbox.push_back(std::move(cand));
  }
}

void RxOnTrack(lw_transceiver_t* transceiver, void* user) {
  auto* rx = static_cast<Receiver*>(user);
  // on_track hands over an owning handle.
  lw_receiver_t* receiver = lw_transceiver_receiver(transceiver);
  lw_release(transceiver);
  if (receiver == nullptr) {
    return;
  }
  rx->track = lw_receiver_video_track(receiver);
  lw_release(receiver);
  if (rx->track == nullptr) {
    return;
  }
  // The handle keeps the track alive, so the binding outlives this callback.
  const int rc = lw_video_track_bind_sink(rx->track, rx->token);
  lw_video_track_set_frame_callback(rx->track, OnAnyFrame, nullptr);
  std::printf("  remote track bound to native sink (rc=%d)\n", rc);
}

// SDP callbacks copy their borrowed strings before returning.
struct SdpResult {
  std::string sdp;
  std::atomic<bool> done{false};
};

void OnSdp(const char* sdp, const char* /*type*/, void* user) {
  auto* result = static_cast<SdpResult*>(user);
  result->sdp = sdp != nullptr ? sdp : "";
  result->done = true;
}

void OnSdpFailure(const char* error, void* user) {
  std::printf("  sdp failure: %s\n", error != nullptr ? error : "?");
  static_cast<SdpResult*>(user)->done = true;
}

void OnSetDone(void* user) { *static_cast<std::atomic<bool>*>(user) = true; }

void OnSetFailure(const char* error, void* user) {
  std::printf("  set description failure: %s\n",
              error != nullptr ? error : "?");
  *static_cast<std::atomic<bool>*>(user) = true;
}

std::string g_sdp;
std::atomic<bool> g_sdp_done{false};
std::atomic<bool> g_set_done{false};
// A callback may still be in flight if a wait times out, so the state it
// writes through outlives every wait.
std::atomic<bool> g_c_set_done{false};
SdpResult g_answer;

bool Wait(std::atomic<bool>* flag, int timeout_ms = 10000) {
  for (int i = 0; i < timeout_ms / 10 && !*flag; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return flag->load();
}

}  // namespace

int main() {
  // Opt the hardware decoder in; without this the builtin software decoder
  // runs and no native buffer is ever produced.
  setenv("LW_V4L2", "1", 1);
  if (std::getenv("LW_E2E_VERBOSE") != nullptr) {
    LibWebRTCLogging::setLogSink(RTCLoggingSeverity::Info, [](const string& m) {
      std::fputs(m.std_string().c_str(), stderr);
    });
  }

  if (!LibWebRTC::Initialize()) {
    std::printf("RESULT: FAIL (initialize)\n");
    return 1;
  }
  scoped_refptr<RTCPeerConnectionFactory> factory =
      LibWebRTC::CreateRTCPeerConnectionFactory();
  if (!factory || !factory->Initialize()) {
    std::printf("RESULT: FAIL (factory)\n");
    return 1;
  }

  LwVideoSinkV1 sink{};
  sink.size = sizeof(sink);
  sink.on_frame = OnFrame;
  sink.on_format = OnFormat;
  sink.on_eos = OnEos;
  const lw_video_sink_token token = lw_video_sink_register(&sink, nullptr);
  if (token == 0) {
    std::printf("RESULT: FAIL (sink register)\n");
    return 1;
  }

  RTCConfiguration config;
  scoped_refptr<RTCMediaConstraints> constraints =
      RTCMediaConstraints::Create();

  lw_factory_t* c_factory = lw_factory_create();
  if (c_factory == nullptr || lw_factory_initialize(c_factory) == 0) {
    std::printf("RESULT: FAIL (c factory)\n");
    return 1;
  }

  Peer sender;
  Receiver receiver;
  sender.name = "sender";
  receiver.token = token;
  receiver.sender = &sender;
  sender.pc = factory->Create(config, constraints);
  receiver.pc = lw_pc_create(c_factory);
  if (!sender.pc || receiver.pc == nullptr) {
    std::printf("RESULT: FAIL (peer connection)\n");
    return 1;
  }
  sender.deliver = [&receiver](const Candidate& c) { receiver.Deliver(c); };
  sender.peer_ready = &receiver.remote_set;
  sender.pc->RegisterRTCPeerConnectionObserver(&sender);

  LwPcObserver rx_observer{};
  rx_observer.size = sizeof(rx_observer);
  rx_observer.on_connection_state = RxOnConnectionState;
  rx_observer.on_ice_candidate = RxOnIceCandidate;
  rx_observer.on_track = RxOnTrack;
  if (lw_pc_set_observer(receiver.pc, &rx_observer, &receiver) != 0) {
    std::printf("RESULT: FAIL (observer)\n");
    return 1;
  }

  scoped_refptr<RTCVideoSource> source =
      factory->CreateCustomVideoSource("e2e-source", constraints);
  scoped_refptr<RTCVideoTrack> track =
      factory->CreateVideoTrack(source, "e2e-video");
  std::vector<string> stream_ids;
  stream_ids.push_back(string("e2e-stream"));
  sender.pc->AddTrack(track, vector<string>(stream_ids));

  // ---- offer / answer, restricted to H.264 --------------------------------
  g_sdp_done = false;
  sender.pc->CreateOffer(
      [](const string& sdp, const string&) {
        g_sdp = ForceH264(sdp.std_string());
        g_sdp_done = true;
      },
      [](const char*) { g_sdp_done = true; }, constraints);
  if (!Wait(&g_sdp_done) || g_sdp.empty() ||
      g_sdp.find("H264") == std::string::npos) {
    std::printf("RESULT: FAIL (no H.264 offer)\n");
    return 1;
  }

  auto set_cpp_description = [&](bool local, const std::string& sdp,
                                 const char* type) {
    g_set_done = false;
    if (local) {
      sender.pc->SetLocalDescription(
          sdp.c_str(), type, [] { g_set_done = true; },
          [](const char*) { g_set_done = true; });
    } else {
      sender.pc->SetRemoteDescription(
          sdp.c_str(), type, [] { g_set_done = true; },
          [](const char*) { g_set_done = true; });
    }
    return Wait(&g_set_done);
  };

  auto set_c_description = [&](bool local, const std::string& sdp,
                               const char* type) {
    g_c_set_done = false;
    if (local) {
      lw_pc_set_local_description(receiver.pc, sdp.c_str(), type, OnSetDone,
                                  OnSetFailure, &g_c_set_done);
    } else {
      lw_pc_set_remote_description(receiver.pc, sdp.c_str(), type, OnSetDone,
                                   OnSetFailure, &g_c_set_done);
    }
    return Wait(&g_c_set_done);
  };

  set_cpp_description(true, g_sdp, "offer");
  set_c_description(false, g_sdp, "offer");
  receiver.remote_set = true;
  sender.FlushCandidates();

  lw_pc_create_answer(receiver.pc, OnSdp, OnSdpFailure, &g_answer);
  if (!Wait(&g_answer.done) || g_answer.sdp.empty()) {
    std::printf("RESULT: FAIL (no answer)\n");
    return 1;
  }
  set_c_description(true, g_answer.sdp, "answer");
  set_cpp_description(false, g_answer.sdp, "answer");
  sender.remote_set = true;
  receiver.FlushCandidates();

  // ---- feed frames until the sink has enough, or we give up ---------------
  std::vector<uint8_t> i420(static_cast<size_t>(kWidth) * kHeight * 3 / 2);
  for (int n = 0; n < 300 && g_frames < kWantFrames; ++n) {
    for (int y = 0; y < kHeight; ++y) {
      for (int x = 0; x < kWidth; ++x) {
        i420[static_cast<size_t>(y) * kWidth + x] =
            static_cast<uint8_t>(x + y + n * 3);
      }
    }
    std::memset(i420.data() + static_cast<size_t>(kWidth) * kHeight, 128,
                static_cast<size_t>(kWidth) * kHeight / 2);
    source->OnCapturedFrame(RTCVideoFrame::Create(
        kWidth, kHeight, i420.data(), static_cast<int>(i420.size())));
    std::this_thread::sleep_for(std::chrono::milliseconds(33));
  }

  // Let anything in flight land before the counters are compared.
  std::this_thread::sleep_for(std::chrono::milliseconds(300));

  const int frames = g_frames.load();
  const int formats = g_formats.load();
  const int bad = g_bad.load();
  const int cpu = g_delivered.load() - frames;
  const int early = g_frames_before_format.load();
  std::printf(
      "frames=%d formats=%d malformed=%d cpu_path=%d before_format=%d "
      "connected=%d\n",
      frames, formats, bad, cpu, early, static_cast<int>(g_connected));

  const bool pass = frames >= kWantFrames / 2 && formats >= 1 && bad == 0 &&
                    cpu == 0 && early == 0;
  std::printf("RESULT: %s\n", pass ? "PASS" : "FAIL");

  if (receiver.track != nullptr) {
    lw_video_track_unbind_sink(receiver.track);
    lw_release(receiver.track);
  }
  lw_video_sink_unregister(token);
  sender.pc->Close();
  lw_pc_remove_observer(receiver.pc);
  lw_pc_close(receiver.pc);
  // Released before the factory it came from, and safe in either order.
  lw_release(receiver.pc);
  lw_release(c_factory);
  LibWebRTC::Terminate();
  return pass ? 0 : 1;
}
