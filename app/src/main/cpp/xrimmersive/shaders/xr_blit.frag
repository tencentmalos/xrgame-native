#version 450
// Composite shader for the Vulkan XR path (the GLES path's quad and projection programs).
//   mode 0: one source; uv0 = {offset.xy, scale.xy} selects the crop (negative scale flips).
//   mode 1: 2D quad: the game image inside the content rectangle, the overlay over the whole quad
//           at its own alpha (kDirectQuadFragmentShader in xr_immersive.cpp).
// Sources hold sRGB-encoded bytes in UNORM images; with decodeSrgb the shader returns linear
// values so an sRGB attachment stores the original bytes again.
layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D uSource;
layout(set = 0, binding = 1) uniform sampler2D uOverlay;

layout(push_constant) uniform Params {
    vec4 uv0;
    vec2 contentScale;
    uint mode;
    uint flags;
} params;

const uint kDecodeSrgb = 1u;
const uint kOpaque = 2u;

vec3 srgbToLinear(vec3 c) {
    bvec3 low = lessThanEqual(c, vec3(0.04045));
    vec3 lowPart = c / 12.92;
    vec3 highPart = pow((c + 0.055) / 1.055, vec3(2.4));
    return mix(highPart, lowPart, vec3(low));
}

vec3 decode(vec3 c) {
    return (params.flags & kDecodeSrgb) != 0u ? srgbToLinear(c) : c;
}

void main() {
    if (params.mode == 1u) {
        vec2 gameUv = (vUv - 0.5) / params.contentScale + 0.5;
        bool insideGame = all(greaterThanEqual(gameUv, vec2(0.0))) && all(lessThanEqual(gameUv, vec2(1.0)));
        vec4 game = insideGame ? texture(uSource, gameUv) : vec4(0.0);
        vec4 overlay = texture(uOverlay, vUv);
        vec3 rgb = mix(decode(game.rgb), decode(overlay.rgb), overlay.a);
        outColor = vec4(rgb, insideGame ? 1.0 : overlay.a);
        return;
    }
    vec4 c = texture(uSource, params.uv0.xy + vUv * params.uv0.zw);
    outColor = vec4(decode(c.rgb), (params.flags & kOpaque) != 0u ? 1.0 : c.a);
}
