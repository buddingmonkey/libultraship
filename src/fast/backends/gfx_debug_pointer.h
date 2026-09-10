#pragma once

#ifdef ENABLE_DEBUG_TOOLS

namespace Fast::DebugPointer {

// Write "<u> <v> [down] [ms=<n>]" to <appdir>/debug-pointer to place the pointer, in picture coordinates 0 to 1.
bool Poll(float* u, float* v, bool* down);

} // namespace Fast::DebugPointer

#endif
