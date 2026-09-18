#version 450
#extension GL_GOOGLE_include_directive : require

// Identical to mesh.frag apart from dropping the G-buffer outputs: the
// transparent pass draws into the lit image alone.
#define MIRABILIS_COLOR_ONLY 1
#include "mesh_shading.glsl"
