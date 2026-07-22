/*
 * lw_string.h — string payloads handed to callbacks.
 *
 * Internal to the C shim; not installed.
 */
#ifndef LW_STRING_H_
#define LW_STRING_H_

namespace lw {

// Copies `s` into a buffer the callee owns and retires with lw_string_free.
// Returns null for a null input, or if the copy cannot be allocated: a
// callback that loses its payload is better than one that is never delivered.
char* DupString(const char* s);

}  // namespace lw

#endif  // LW_STRING_H_
