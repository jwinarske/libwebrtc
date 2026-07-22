#include "src/c/lw_handle.h"

namespace lw {

Handle::Handle(void* raw,
               libwebrtc::scoped_refptr<libwebrtc::RefCountInterface> object,
               libwebrtc::scoped_refptr<libwebrtc::RefCountInterface> producer)
    : raw_(raw), producer_(std::move(producer)), object_(std::move(object)) {}

Handle::~Handle() = default;

int Handle::AddRef() const {
  return ref_count_.fetch_add(1, std::memory_order_relaxed) + 1;
}

int Handle::Release() const {
  const int count = ref_count_.fetch_sub(1, std::memory_order_acq_rel) - 1;
  if (count == 0) {
    delete this;
  }
  return count;
}

libwebrtc::scoped_refptr<libwebrtc::RefCountInterface> Handle::producer()
    const {
  return producer_.get() ? producer_ : object_;
}

}  // namespace lw
