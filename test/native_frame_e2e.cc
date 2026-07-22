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
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "c/lw_c_api.h"
#include "libwebrtc.h"
#include "rtc_logging.h"

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

// A peer, driven entirely through the C ABI. Both ends of the loopback are
// built this way, so the test covers the surface a foreign consumer sees
// rather than the C++ interface underneath it.
struct CPeer {
  const char* name = "";
  lw_pc_t* pc = nullptr;
  CPeer* peer = nullptr;
  bool receives = false;
  lw_video_sink_token token = 0;
  lw_video_track_t* remote_track = nullptr;
  std::atomic<bool> remote_set{false};

  // A candidate is only accepted once the far side has a remote description;
  // buffer until then or it is silently dropped.
  void Send(const Candidate& c) {
    if (peer->remote_set) {
      lw_pc_add_ice_candidate(peer->pc, c.mid.c_str(), c.index,
                              c.candidate.c_str());
    } else {
      std::lock_guard<std::mutex> lock(mu);
      outbox.push_back(c);
    }
  }

  void FlushCandidates() {
    std::lock_guard<std::mutex> lock(mu);
    for (const Candidate& c : outbox) {
      lw_pc_add_ice_candidate(peer->pc, c.mid.c_str(), c.index,
                              c.candidate.c_str());
    }
    outbox.clear();
  }

  std::mutex mu;
  std::vector<Candidate> outbox;
};

void OnConnectionState(int state, void*) {
  if (state == LW_PC_STATE_CONNECTED) {
    g_connected = true;
  }
}

void OnIceCandidate(char* candidate, char* mid, int mline_index, void* user) {
  Candidate cand{.mid = mid != nullptr ? mid : "",
                 .index = mline_index,
                 .candidate = candidate != nullptr ? candidate : ""};
  lw_string_free(candidate);
  lw_string_free(mid);
  static_cast<CPeer*>(user)->Send(cand);
}

void OnTrack(lw_transceiver_t* transceiver, void* user) {
  auto* self = static_cast<CPeer*>(user);
  // on_track hands over an owning handle.
  lw_receiver_t* receiver = lw_transceiver_receiver(transceiver);
  lw_release(transceiver);
  if (receiver == nullptr) {
    return;
  }
  self->remote_track = lw_receiver_video_track(receiver);
  lw_release(receiver);
  if (self->remote_track == nullptr) {
    return;
  }
  // The handle keeps the track alive, so the binding outlives this callback.
  const int rc = lw_video_track_bind_sink(self->remote_track, self->token);
  lw_video_track_set_frame_callback(self->remote_track, OnAnyFrame, nullptr);
  std::printf("  remote track bound to native sink (rc=%d, enabled=%d)\n", rc,
              lw_video_track_enabled(self->remote_track));
}

// SDP callbacks take ownership of their strings.
struct SdpResult {
  std::string sdp;
  std::atomic<bool> done{false};
};

void OnSdp(char* sdp, char* type, void* user) {
  auto* result = static_cast<SdpResult*>(user);
  result->sdp = sdp != nullptr ? sdp : "";
  lw_string_free(sdp);
  lw_string_free(type);
  result->done = true;
}

void OnSdpFailure(char* error, void* user) {
  std::printf("  sdp failure: %s\n", error != nullptr ? error : "?");
  lw_string_free(error);
  static_cast<SdpResult*>(user)->done = true;
}

void OnSetDone(void* user) { *static_cast<std::atomic<bool>*>(user) = true; }

void OnSetFailure(char* error, void* user) {
  std::printf("  set description failure: %s\n",
              error != nullptr ? error : "?");
  lw_string_free(error);
  *static_cast<std::atomic<bool>*>(user) = true;
}

std::string g_sdp;
// A callback may still be in flight if a wait times out, so the state it
// writes through outlives every wait.
std::atomic<bool> g_c_set_done{false};
SdpResult g_offer;
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

  if (lw_initialize() == 0) {
    std::printf("RESULT: FAIL (initialize)\n");
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

  lw_factory_t* factory = lw_factory_create();
  if (factory == nullptr || lw_factory_initialize(factory) == 0) {
    std::printf("RESULT: FAIL (factory)\n");
    return 1;
  }

  CPeer sender;
  CPeer receiver;
  sender.name = "sender";
  receiver.name = "receiver";
  sender.peer = &receiver;
  receiver.peer = &sender;
  receiver.receives = true;
  receiver.token = token;
  sender.pc = lw_pc_create(factory);
  receiver.pc = lw_pc_create(factory);
  if (sender.pc == nullptr || receiver.pc == nullptr) {
    std::printf("RESULT: FAIL (peer connection)\n");
    return 1;
  }

  LwPcObserver observer{};
  observer.size = sizeof(observer);
  observer.on_connection_state = OnConnectionState;
  observer.on_ice_candidate = OnIceCandidate;
  observer.on_track = OnTrack;
  if (lw_pc_set_observer(sender.pc, &observer, &sender) != 0 ||
      lw_pc_set_observer(receiver.pc, &observer, &receiver) != 0) {
    std::printf("RESULT: FAIL (observer)\n");
    return 1;
  }

  lw_video_source_t* source =
      lw_factory_create_video_source(factory, "e2e-source");
  lw_video_track_t* track =
      lw_factory_create_video_track(factory, source, "e2e-video");
  const char* stream_ids[] = {"e2e-stream"};
  lw_sender_t* rtp_sender = lw_pc_add_track(sender.pc, track, stream_ids, 1);
  if (source == nullptr || track == nullptr || rtp_sender == nullptr) {
    std::printf("RESULT: FAIL (local video)\n");
    return 1;
  }
  if (lw_video_track_enabled(track) != 1) {
    std::printf("RESULT: FAIL (track not enabled by default)\n");
    return 1;
  }

  // ---- offer / answer, restricted to H.264 --------------------------------
  lw_pc_create_offer(sender.pc, OnSdp, OnSdpFailure, &g_offer);
  if (!Wait(&g_offer.done) || g_offer.sdp.empty()) {
    std::printf("RESULT: FAIL (no offer)\n");
    return 1;
  }
  g_sdp = ForceH264(g_offer.sdp);
  if (g_sdp.find("H264") == std::string::npos) {
    std::printf("RESULT: FAIL (no H.264 offer)\n");
    return 1;
  }

  auto set_description = [](lw_pc_t* pc, bool local, const std::string& sdp,
                            const char* type) {
    g_c_set_done = false;
    if (local) {
      lw_pc_set_local_description(pc, sdp.c_str(), type, OnSetDone,
                                  OnSetFailure, &g_c_set_done);
    } else {
      lw_pc_set_remote_description(pc, sdp.c_str(), type, OnSetDone,
                                   OnSetFailure, &g_c_set_done);
    }
    return Wait(&g_c_set_done);
  };

  set_description(sender.pc, true, g_sdp, "offer");
  set_description(receiver.pc, false, g_sdp, "offer");
  receiver.remote_set = true;
  sender.FlushCandidates();

  lw_pc_create_answer(receiver.pc, OnSdp, OnSdpFailure, &g_answer);
  if (!Wait(&g_answer.done) || g_answer.sdp.empty()) {
    std::printf("RESULT: FAIL (no answer)\n");
    return 1;
  }
  set_description(receiver.pc, true, g_answer.sdp, "answer");
  set_description(sender.pc, false, g_answer.sdp, "answer");
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
    if (lw_video_source_push_i420(source, kWidth, kHeight, i420.data(),
                                  i420.size()) != 0) {
      std::printf("RESULT: FAIL (push frame)\n");
      return 1;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(33));
  }

  // Mute and unmute the local track. A disabled track keeps flowing, so this
  // checks the state round-trips rather than expecting frames to stop.
  const bool mute_ok = lw_video_track_set_enabled(track, 0) == 0 &&
                       lw_video_track_enabled(track) == 0 &&
                       lw_video_track_set_enabled(track, 1) == 0 &&
                       lw_video_track_enabled(track) == 1;

  // Let anything in flight land before the counters are compared.
  std::this_thread::sleep_for(std::chrono::milliseconds(300));

  const int frames = g_frames.load();
  const int formats = g_formats.load();
  const int bad = g_bad.load();
  const int cpu = g_delivered.load() - frames;
  const int early = g_frames_before_format.load();
  std::printf(
      "frames=%d formats=%d malformed=%d cpu_path=%d before_format=%d "
      "connected=%d mute=%d\n",
      frames, formats, bad, cpu, early, static_cast<int>(g_connected),
      static_cast<int>(mute_ok));

  const bool pass = frames >= kWantFrames / 2 && formats >= 1 && bad == 0 &&
                    cpu == 0 && early == 0 && mute_ok;
  std::printf("RESULT: %s\n", pass ? "PASS" : "FAIL");

  if (receiver.remote_track != nullptr) {
    lw_video_track_unbind_sink(receiver.remote_track);
    lw_release(receiver.remote_track);
  }
  lw_video_sink_unregister(token);
  lw_pc_remove_observer(sender.pc);
  lw_pc_remove_observer(receiver.pc);
  lw_pc_close(sender.pc);
  lw_pc_close(receiver.pc);
  // Released before the factory they came from, and safe in either order.
  lw_release(rtp_sender);
  lw_release(track);
  lw_release(source);
  lw_release(sender.pc);
  lw_release(receiver.pc);
  lw_release(factory);
  lw_terminate();
  return pass ? 0 : 1;
}
