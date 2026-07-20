/*
 * lw_c_api.h — flat C control-plane ABI for libwebrtc.so.
 *
 * Dart-loadable (dart:ffi) and C++-consumable surface over the subset of the
 * wrapper the data plane needs. This first slice covers the video sink
 * registry: presenters register an LwVideoSinkV1 and bind it to a video track
 * by an unguessable token, after which native (dmabuf) frames flow to the sink
 * native-to-native (see lw_video_sink.h). More of the control plane is added
 * here incrementally.
 */
#ifndef LW_C_API_H_
#define LW_C_API_H_

#include <stdint.h>

#include "lw_video_sink.h"

#if defined(_WIN32)
#define LW_C_API __declspec(dllexport)
#else
#define LW_C_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque handle to a video track, produced by the control-plane shim. */
typedef struct lw_video_track lw_video_track_t;

/* Unguessable, nonzero handle to a registered video sink. 0 is invalid. */
typedef uint64_t lw_video_sink_token;

/* Registers a native video sink. `sink` and `user` are caller-owned and MUST
 * outlive the binding (they are referenced, not copied). Returns an
 * unguessable token, or 0 on failure (null/invalid sink). */
LW_C_API lw_video_sink_token lw_video_sink_register(const LwVideoSinkV1* sink,
                                                    void* user);

/* Unregisters a sink token. Returns 0 on success, negative if unknown. Does
 * not unbind: unbind the track first (lw_video_track_unbind_sink). */
LW_C_API int lw_video_sink_unregister(lw_video_sink_token token);

/* Binds a registered sink to a video track; native frames on the track are
 * then delivered to the sink. Returns 0 on success, negative on error
 * (null/unknown track, unknown token). */
LW_C_API int lw_video_track_bind_sink(lw_video_track_t* track,
                                      lw_video_sink_token token);

/* Unbinds any native sink from a track. Quiesces: blocks until an in-flight
 * on_frame returns, and emits on_eos to the previously-bound sink. Returns 0
 * on success, negative on error. */
LW_C_API int lw_video_track_unbind_sink(lw_video_track_t* track);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* LW_C_API_H_ */
