#pragma once

#ifdef ENABLE_DEBUG_TOOLS

namespace Fast::DebugPointer {

// Write "<u> <v> [down] [ms=<n>]" to <appdir>/debug-pointer to place the pointer, in picture coordinates 0 to 1.
bool Poll(float* u, float* v, bool* down);

// Same format in <appdir>/debug-keyboard: a ray onto the headset keyboard, 0 to 1 from its top left corner.
bool PollKeyboard(float* u, float* v, bool* down);

} // namespace Fast::DebugPointer

#endif
