// The alpha cutoff test, kept out of input_structures.glsl because discard is
// a fragment-stage operation and that header is included by vertex shaders
// too.  Include this only from a .frag.
//
// glTF defines MASK as a hard binary test rather than a blend: a fragment is
// either fully present or entirely absent, which is what lets masked geometry
// keep writing depth exactly like an opaque surface.
void apply_alpha_mask(float alpha)
{
    if (alpha < materialData.alphaMask.x) {
        discard;
    }
}
