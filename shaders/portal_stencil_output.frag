#version 450
layout(location = 0) in vec2 inClipDepth;
layout(location = 0) out float outMask;

void main()
{
    if (inClipDepth.x < 0.0 || inClipDepth.y <= 0.0) discard;
    // The original projective depth is interpolated before clamping, so the
    // aperture survives an oblique near-plane crossing and still tests against
    // foreground geometry at its real depth (reversed near=1, far=0).
    gl_FragDepth = clamp(inClipDepth.x / inClipDepth.y, 0.0, 1.0);
    outMask = 1.0;
}
