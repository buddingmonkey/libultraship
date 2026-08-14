#pragma once

namespace Fast {

// Where the head is, measured from the viewpoint the game's own projection was made for, and how
// far that viewpoint sits from the window plane. Both in game units, in the window's axes: x
// right, y up, z towards the viewer. The interpreter turns the two into an off-axis frustum.
// Declared apart from gfx_openxr.h so the interpreter needs none of the OpenXR headers, and so a
// second headset backend can fill the same numbers.
struct XrViewGeometry {
    float eyeOffset[3];
    float windowDistance;
};

bool GetXrViewGeometry(XrViewGeometry* geometry);

// The half-angle tangents the game asks for, after the widescreen adjustment. The backend sizes
// the window to match, so the framing does not change when the off-axis frustum replaces the
// game's own projection.
void SetXrViewTangents(float tanHalfWidth, float tanHalfHeight);

// Radians the window covers, for a caller that scales a menu drawn on it. Zero without a session.
float GetXrWindowAngularWidth();

} // namespace Fast
