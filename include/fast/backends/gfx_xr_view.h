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

// How far the window hangs from the viewer, in metres. The window keeps the angular size the
// game's field of view gives it, so this does not change the framing: it scales the whole diorama
// towards the viewer, and a smaller world at a shorter range has more depth in it. Parallax on the
// glass never exceeds one eye separation whatever the distance, so no setting here can make the
// eyes diverge.
void SetXrWindowDistance(float metres);

// Puts the window in front of the viewer, where they are looking now.
void RecentreXrWindow();

// Draws one image for both eyes instead of one per eye. There is no parallax then, but the frame
// costs half as much and only one layer reaches the compositor.
void SetXrStereo(bool enabled);

// Holds the next draws on the glass. A HUD element is placed in front of the camera by the game
// and is not part of the world behind the window, so it must not take the off-axis frustum: with
// this set the game's own projection stands, both eyes draw the same picture, and the element
// lands on the window plane with no parallax.
void SetXrFlatProjection(bool flat);

// Radians the window covers, for a caller that scales a menu drawn on it. Zero without a session.
float GetXrWindowAngularWidth();

} // namespace Fast
