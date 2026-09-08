#version 450

layout(location = 0) out vec4 outFragColor;

// The finished scene colour, with a linear clamp-to-edge sampler.  The blend
// at the end of the filter is a single bilinear tap, so the hardware does the
// mixing and the HDR values in this image are never manually recombined.
layout(set = 0, binding = 0) uniform sampler2D sceneColor;

layout(push_constant) uniform constants {
    // xy = one texel in UV space.  It comes from the draw image allocation,
    // not the rendered region, because that is what a step of one pixel means
    // in this texture.  z = edge contrast threshold, w = subpixel aliasing
    // removal strength.
    vec4 settings;
    // xy = the largest UV the render scale actually rendered this frame, so
    // an edge search never walks into the untouched part of the image.
    // z = 1 while detected edges replace the image.
    vec4 limits;
} PushConstants;

// How far the edge search may travel, and how quickly it accelerates once the
// first few single-texel steps have not found the end of the edge.  These are
// the FXAA 3.11 quality preset steps.
const int SearchSteps = 12;
const float SearchStep[SearchSteps] = float[](
    1.0, 1.0, 1.0, 1.0, 1.0, 1.5, 2.0, 2.0, 2.0, 2.0, 4.0, 8.0);

// Below this absolute contrast a pixel is treated as flat regardless of the
// relative threshold, which keeps the filter off smooth gradients.
const float EdgeThresholdMin = 0.0312;

vec3 sample_scene(vec2 uv)
{
    return textureLod(
        sceneColor, clamp(uv, vec2(0.0), PushConstants.limits.xy), 0.0).rgb;
}

// Edges are found in a compressed approximation of the displayed image rather
// than in raw HDR.  A bright highlight against a wall is a step of a few
// hundred in linear light, and every contrast test would saturate on it.
float edge_luma(vec3 hdr)
{
    vec3 display = hdr / (hdr + vec3(1.0));
    return dot(display, vec3(0.2126, 0.7152, 0.0722));
}

void main()
{
    vec2 texel = PushConstants.settings.xy;
    float edgeThreshold = PushConstants.settings.z;
    float subpixStrength = PushConstants.settings.w;
    bool showEdges = PushConstants.limits.z > 0.5;
    // The centre of the pixel this invocation covers, in the texture's own
    // coordinates.  The image is allocated at window size and only partly
    // rendered when the resolution scale is below 1, so this cannot come from
    // an interpolated fullscreen coordinate.
    vec2 posM = gl_FragCoord.xy * texel;

    vec3 rgbM = sample_scene(posM);
    float lumaM = edge_luma(rgbM);
    float lumaN = edge_luma(sample_scene(posM + vec2(0.0, -texel.y)));
    float lumaS = edge_luma(sample_scene(posM + vec2(0.0,  texel.y)));
    float lumaW = edge_luma(sample_scene(posM + vec2(-texel.x, 0.0)));
    float lumaE = edge_luma(sample_scene(posM + vec2( texel.x, 0.0)));

    float lumaMax = max(max(lumaN, lumaW), max(lumaE, max(lumaS, lumaM)));
    float lumaMin = min(min(lumaN, lumaW), min(lumaE, min(lumaS, lumaM)));
    float lumaRange = lumaMax - lumaMin;

    // Flat enough to leave alone.  Most of a frame takes this path, which is
    // what makes the filter cheap.
    if (lumaRange < max(EdgeThresholdMin, lumaMax * edgeThreshold)) {
        outFragColor = vec4(showEdges ? vec3(0.0) : rgbM, 1.0);
        return;
    }
    if (showEdges) {
        outFragColor = vec4(1.0);
        return;
    }

    float lumaNW = edge_luma(sample_scene(posM + vec2(-texel.x, -texel.y)));
    float lumaNE = edge_luma(sample_scene(posM + vec2( texel.x, -texel.y)));
    float lumaSW = edge_luma(sample_scene(posM + vec2(-texel.x,  texel.y)));
    float lumaSE = edge_luma(sample_scene(posM + vec2( texel.x,  texel.y)));

    // Second derivatives across the 3x3 neighbourhood.  Whichever direction
    // carries the larger one is the direction the edge runs along.
    float lumaNS = lumaN + lumaS;
    float lumaWE = lumaW + lumaE;
    float lumaNWNE = lumaNW + lumaNE;
    float lumaSWSE = lumaSW + lumaSE;
    float lumaNWSW = lumaNW + lumaSW;
    float lumaNESE = lumaNE + lumaSE;

    float edgeHorz =
        abs(-2.0 * lumaW + lumaNWSW) +
        abs(-2.0 * lumaM + lumaNS) * 2.0 +
        abs(-2.0 * lumaE + lumaNESE);
    float edgeVert =
        abs(-2.0 * lumaS + lumaSWSE) +
        abs(-2.0 * lumaM + lumaWE) * 2.0 +
        abs(-2.0 * lumaN + lumaNWNE);
    bool horzSpan = edgeHorz >= edgeVert;

    // The subpixel term: how far this pixel's luma sits from the average of
    // its neighbours.  It handles single-pixel features an edge search cannot
    // resolve, such as a thin pole or a specular sparkle.
    float subpixA = (lumaNS + lumaWE) * 2.0 + lumaNWSW + lumaNESE;
    float subpixB = abs(subpixA * (1.0 / 12.0) - lumaM) / lumaRange;
    float subpixBlend = smoothstep(0.0, 1.0, clamp(subpixB, 0.0, 1.0));
    subpixBlend = subpixBlend * subpixBlend * subpixStrength;

    // Step perpendicular to the edge, towards whichever neighbour differs
    // more.  That neighbour and this pixel straddle the true edge.
    float lumaPerpN = horzSpan ? lumaN : lumaW;
    float lumaPerpS = horzSpan ? lumaS : lumaE;
    float gradientN = lumaPerpN - lumaM;
    float gradientS = lumaPerpS - lumaM;
    bool pairN = abs(gradientN) >= abs(gradientS);
    float gradientScaled = max(abs(gradientN), abs(gradientS)) * 0.25;

    // Which way the half-pixel step and the final blend travel.  N is up the
    // screen and W is left, so both are the negative texel direction.
    float lengthSign = horzSpan ? texel.y : texel.x;
    if (pairN) {
        lengthSign = -lengthSign;
    }
    // The average of the two straddling pixels: the luma of the edge itself,
    // and the value the search compares against.
    float lumaLocalAverage = 0.5 * ((pairN ? lumaPerpN : lumaPerpS) + lumaM);

    // Walk along the edge from a point half a pixel into it, in both
    // directions, until the local luma stops matching the edge.
    vec2 posB = posM;
    if (horzSpan) {
        posB.y += lengthSign * 0.5;
    } else {
        posB.x += lengthSign * 0.5;
    }
    vec2 offset = horzSpan ? vec2(texel.x, 0.0) : vec2(0.0, texel.y);

    vec2 posNeg = posB - offset * SearchStep[0];
    vec2 posPos = posB + offset * SearchStep[0];
    float lumaEndNeg = edge_luma(sample_scene(posNeg)) - lumaLocalAverage;
    float lumaEndPos = edge_luma(sample_scene(posPos)) - lumaLocalAverage;
    bool doneNeg = abs(lumaEndNeg) >= gradientScaled;
    bool donePos = abs(lumaEndPos) >= gradientScaled;

    for (int step = 1; step < SearchSteps; ++step) {
        if (doneNeg && donePos) {
            break;
        }
        if (!doneNeg) {
            posNeg -= offset * SearchStep[step];
            lumaEndNeg = edge_luma(sample_scene(posNeg)) - lumaLocalAverage;
            doneNeg = abs(lumaEndNeg) >= gradientScaled;
        }
        if (!donePos) {
            posPos += offset * SearchStep[step];
            lumaEndPos = edge_luma(sample_scene(posPos)) - lumaLocalAverage;
            donePos = abs(lumaEndPos) >= gradientScaled;
        }
    }

    float distanceNeg = horzSpan ? (posM.x - posNeg.x) : (posM.y - posNeg.y);
    float distancePos = horzSpan ? (posPos.x - posM.x) : (posPos.y - posM.y);
    float spanLength = distanceNeg + distancePos;

    // How far along its own span this pixel sits.  One near the middle of a
    // long span is barely moved; one at an end is moved almost half a pixel,
    // which is what turns a staircase back into a straight line.
    bool nearestIsNeg = distanceNeg < distancePos;
    float pixelOffset =
        0.5 - min(distanceNeg, distancePos) / max(spanLength, 1e-6);

    // Only trust that offset if the end found on the nearer side really is
    // the end of this edge, rather than a second edge the search ran into.
    bool centerIsBelow = (lumaM - lumaLocalAverage) < 0.0;
    bool goodSpan =
        ((nearestIsNeg ? lumaEndNeg : lumaEndPos) < 0.0) != centerIsBelow;
    float finalOffset = max(goodSpan ? pixelOffset : 0.0, subpixBlend);

    // A single bilinear tap between this pixel and its neighbour across the
    // edge.  Sampling the original image keeps the result in HDR; only the
    // decisions above ran on the compressed luma.
    vec2 posFinal = posM;
    if (horzSpan) {
        posFinal.y += finalOffset * lengthSign;
    } else {
        posFinal.x += finalOffset * lengthSign;
    }
    outFragColor = vec4(sample_scene(posFinal), 1.0);
}
