#version 450

// One oversized triangle covering the viewport.  The fragment shader locates
// its texel from gl_FragCoord, so no interpolated coordinate is needed and
// this stage binds nothing at all.
void main()
{
    const vec2 positions[3] = vec2[](
        vec2(-1.0, -1.0),
        vec2( 3.0, -1.0),
        vec2(-1.0,  3.0));

    gl_Position = vec4(positions[gl_VertexIndex], 0.0, 1.0);
}
