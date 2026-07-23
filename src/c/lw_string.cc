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

char* DupBytes(const char* data, size_t size) {
  if (data == nullptr) {
    return nullptr;
  }
  char* copy = static_cast<char*>(std::malloc(size + 1));
  if (copy != nullptr) {
    std::memcpy(copy, data, size);
    copy[size] = '\0';
  }
  return copy;
}

}  // namespace lw
