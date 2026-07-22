/*
 * lw_handle.h — the object behind every opaque lw_* handle.
 *
 * Internal to the C shim; not installed.
 */
#ifndef LW_HANDLE_H_
#define LW_HANDLE_H_

#include <atomic>
#include <utility>

#include "base/refcount.h"
#include "base/scoped_ref_ptr.h"

namespace lw {

// The object a caller asked for, plus a reference to the factory that produced
// it.
//
// The factory owns the signaling, worker and network threads, and the proxies
// inside a peer connection, transceiver or track post to those threads while
// being destroyed. Releasing the factory first therefore tears the threads down
// underneath objects that still need them, which crashes. A caller whose
// cleanup order is not its own to choose -- any garbage-collected binding,
// where finalizers run in an unspecified order -- cannot prevent that, so every
// handle keeps the factory alive instead and the release order stops mattering.
class Handle : public libwebrtc::RefCountInterface {
 public:
  // Wraps `object`, keeping `producer` alive at least as long. Returns null if
  // `object` is null. The returned handle owns one reference.
  template <typename T>
  static Handle* Create(
      libwebrtc::scoped_refptr<T> object,
      libwebrtc::scoped_refptr<libwebrtc::RefCountInterface> producer) {
    if (!object.get()) {
      return nullptr;
    }
    void* raw = object.get();
    return new Handle(raw, std::move(object), std::move(producer));
  }

  Handle(const Handle&) = delete;
  Handle& operator=(const Handle&) = delete;

  int AddRef() const override;
  int Release() const override;

  // The wrapped object, as the type the handle was created from.
  template <typename T>
  T* As() const {
    return static_cast<T*>(raw_);
  }

  // The factory behind this handle, to be passed on to handles derived from
  // it. For the factory's own handle that is the factory itself.
  libwebrtc::scoped_refptr<libwebrtc::RefCountInterface> producer() const;

 private:
  Handle(void* raw,
         libwebrtc::scoped_refptr<libwebrtc::RefCountInterface> object,
         libwebrtc::scoped_refptr<libwebrtc::RefCountInterface> producer);
  ~Handle() override;

  void* const raw_;
  mutable std::atomic<int> ref_count_{1};
  // Declared before object_ so it is destroyed after it: the object may need
  // the factory's threads on the way out.
  const libwebrtc::scoped_refptr<libwebrtc::RefCountInterface> producer_;
  const libwebrtc::scoped_refptr<libwebrtc::RefCountInterface> object_;
};

// The object behind an opaque handle, or null.
template <typename T>
T* From(void* handle) {
  return handle ? reinterpret_cast<Handle*>(handle)->As<T>() : nullptr;
}

// Wraps `object` in a handle tied to the same factory as `parent`.
template <typename T>
Handle* Derive(void* parent, libwebrtc::scoped_refptr<T> object) {
  if (!parent) {
    return nullptr;
  }
  return Handle::Create(std::move(object),
                        reinterpret_cast<Handle*>(parent)->producer());
}

}  // namespace lw

#endif  // LW_HANDLE_H_
