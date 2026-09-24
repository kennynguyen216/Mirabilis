#ifndef MIRABILIS_TRACE_SCENE_GLSL
#define MIRABILIS_TRACE_SCENE_GLSL

// The path tracer's scene records, shared with the surface cache's emitter
// sampling (surface_cache_direct.comp), so both read the same bytes with the
// same meaning.  Layouts match TraceTriangle and TraceMaterial in C++.
struct Triangle { vec4 p0; vec4 p1; vec4 p2; vec4 n0; vec4 n1; vec4 n2; vec4 uv12; uvec4 meta; vec4 c0; vec4 c1; vec4 c2; };
struct Material { vec4 baseColor; vec4 emission; vec4 parameters; vec4 uvScale; uvec4 textureInfo; };

// Emission sidedness has to use one convention in every renderer: the forward
// pass and the surface cache both classify it from the authored normal, so a
// winding-derived test here would let a panel glow in the raster image and
// emit from its other face in the reference trace.
//
// The plane normal is what is returned, not the interpolated vertex normal:
// only its SIGN is taken from the authored normals, because the area-to-solid
// -angle conversion at an area light needs the geometric normal.
vec3 authoredSideNormal(Triangle t,vec3 geometric) {
    vec3 authored=t.n0.xyz+t.n1.xyz+t.n2.xyz;
    return dot(authored,authored)>1e-16&&dot(authored,geometric)<0?-geometric:geometric;
}

#endif
