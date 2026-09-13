#version 450
#extension GL_GOOGLE_include_directive : require

// Identical to mesh.frag apart from the per-fragment alpha test.
#define MIRABILIS_ALPHA_MASK 1
#include "mesh_shading.glsl"
