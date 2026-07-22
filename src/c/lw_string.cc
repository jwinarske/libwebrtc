#include "src/c/lw_string.h"

#include <cstdlib>
#include <cstring>

namespace lw {

char* DupString(const char* s) {
  if (s == nullptr) {
    return nullptr;
  }
  const size_t size = std::strlen(s) + 1;
  char* copy = static_cast<char*>(std::malloc(size));
  if (copy != nullptr) {
    std::memcpy(copy, s, size);
  }
  return copy;
}

}  // namespace lw
