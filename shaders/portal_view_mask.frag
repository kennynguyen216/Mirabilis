#version 450
#extension GL_GOOGLE_include_directive : require

// Identical to portal_view.frag apart from the per-fragment alpha test.
#define MIRABILIS_ALPHA_MASK 1
#include "portal_view_shading.glsl"
