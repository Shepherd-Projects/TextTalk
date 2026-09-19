// Pixel-integrated rolled chrome. Geometry stays fixed; the reflected studio moves.
cbuffer Parameters : register(b0) {
    float4 viewport; // width, height, DPI, continuous optical time
    float4 material; // corner radius, hover, pressed, surface kind
    float4 state;    // switch position, unused, unused, disabled
    float4 sound;
    float4 bands0;
    float4 bands1;
};

float distanceToShape(float2 p, float2 halfSize, float radius) {
    float2 q = abs(p) - halfSize + radius;
    return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - radius;
}

float2 outwardNormal(float2 p, float2 halfSize, float radius) {
    float2 q = abs(p) - halfSize + radius;
    float2 corner = max(q, 0.0);
    float squared = dot(corner, corner);
    float2 direction = squared > 0.000001 ? corner * rsqrt(max(squared, 0.000001))
        : (q.x > q.y ? float2(1, 0) : float2(0, 1));
    return direction * float2(p.x < 0 ? -1 : 1, p.y < 0 ? -1 : 1);
}

float3 rotateStudio(float3 ray, float phase, float hover, float pressed) {
    float yaw = phase * 0.78 + hover * 0.30;
    float tilt = 0.38 * sin(phase * 0.52) + pressed * 0.18;
    float sy, cy, st, ct;
    sincos(yaw, sy, cy); sincos(tilt, st, ct);
    ray.xy = float2(cy * ray.x - sy * ray.y, sy * ray.x + cy * ray.y);
    ray.yz = float2(ct * ray.y - st * ray.z, st * ray.y + ct * ray.z);
    return ray;
}

float softbox(float3 ray, float3 direction, float3 tangent, float width, float length) {
    // Angular area lights have bounded, continuous edges even at grazing angles.
    // No reciprocal ray/plane intersection or screen-space derivatives.
    float forward = dot(ray, direction);
    float across = abs(dot(ray, tangent));
    float along = abs(dot(ray, cross(direction, tangent)));
    float edge = max(across - width, along - length);
    return (1.0 - smoothstep(-0.055, 0.055, edge)) * smoothstep(0.18, 0.45, forward);
}

float studio(float3 ray) {
    const float3 key = float3(-0.60, -0.48, 0.64);
    const float3 keyTangent = float3(0.625, -0.781, 0);
    const float3 strip = float3(0.80, 0, 0.60);
    const float3 stripTangent = float3(0, 1, 0);
    const float3 fill = float3(-0.28, 0.96, 0);
    const float3 fillTangent = float3(0, 0, 1);
    float keyLight = softbox(ray, key, keyTangent, 0.26, 0.72);
    float stripLight = softbox(ray, strip, stripTangent, 0.65, 0.13);
    float fillLight = softbox(ray, fill, fillTangent, 0.30, 0.62);
    float room = 0.07 + 0.07 * saturate(ray.z);
    return room + 0.79 * keyLight + 0.64 * stripLight + 0.43 * fillLight;
}

float3 chrome(float2 p, float distance, float2 halfSize, float radius, float width,
              float phase, float hover, float pressed) {
    float across = saturate(-distance / width);
    // A continuous convex cross-section: inner and outer reflected black bands
    // surround a broad polished crest, rather than two painted white outlines.
    float slope = 1.0 - 2.0 * across;
    float3 normal = float3(outwardNormal(p, halfSize, radius) * slope,
                          sqrt(max(0.0001, 1.0 - slope * slope)));
    float3 viewRay = normalize(float3(p / halfSize * 0.10, -1));
    float3 reflected = rotateStudio(reflect(viewRay, normal), phase, hover, pressed);
    float light = studio(reflected);
    float silver = 0.045 + 0.88 * light;
    // Weak spectral separation belongs to the clear polished lip, not a hue ramp.
    float3 refracted = -refract(viewRay, normal, 1.0 / 1.46);
    float3 spectralRay = rotateStudio(refracted, phase, hover, pressed);
    float red = studio(normalize(spectralRay + normal * 0.012));
    float blue = studio(normalize(spectralRay - normal * 0.012));
    float3 fringe = float3(red - blue, 0, blue - red) * 0.12;
    float edgeShade = 0.78 + 0.22 * sin(across * 3.14159265);
    return saturate(silver.xxx * edgeShade + fringe) * (1.0 - 0.14 * pressed);
}

float4 main(float4 pixel : SV_POSITION, float2 uv : TEXCOORD0) : SV_TARGET {
    float dpi = max(viewport.z, 0.25);
    float2 halfSize = max(viewport.xy * 0.5 - 1.0, 0.01);
    float2 center = uv * viewport.xy - viewport.xy * 0.5;
    float radius = min(material.x, min(halfSize.x, halfSize.y));
    int kind = (int)(material.w + 0.5);
    bool panel = kind == 1;
    bool track = kind == 2;
    bool thumb = kind == 3;
    float hover = material.y * (1.0 - state.w);
    float pressed = material.z * (1.0 - state.w);
    float width = min((panel ? 0.8 : track ? 1.65 : thumb ? 1.5 : 3.0) * dpi,
                      min(halfSize.x, halfSize.y) * 0.35);
    float3 face = panel ? float3(10, 13, 15) / 255.0 : float3(16, 19, 22) / 255.0;
    if (track) face = lerp(float3(7, 10, 13), float3(15, 22, 27), state.x) / 255.0;
    if (thumb) face = float3(0.77, 0.80, 0.83);
    face *= 1.0 - 0.10 * pressed;
    float centerDistance = distanceToShape(center, halfSize, radius);
    // Flat faces need no optical work. Only the narrow rim and its AA fringe
    // receive the sixteen deterministic subpixel samples below.
    if (centerDistance < -width - 1.0) return float4(face, 1);
    if (centerDistance > 1.0) return 0;
    float4 integrated = 0;
    [unroll] for (int y = 0; y < 4; ++y) {
        [unroll] for (int x = 0; x < 4; ++x) {
            float2 p = center + (float2(x, y) + 0.5) * 0.25 - 0.5;
            float d = distanceToShape(p, halfSize, radius);
            // Each sample covers a quarter-pixel footprint, not a whole pixel.
            float outer = saturate(0.5 - d * 4.0);
            float inner = saturate(0.5 - (d + width) * 4.0);
            float3 metal = panel ? float3(0.18, 0.21, 0.24)
                : chrome(p, d, halfSize, radius, width, viewport.w, hover, pressed);
            if (track) metal *= 0.65;
            metal *= 1.0 - 0.45 * state.w;
            integrated += float4(face * inner + metal * max(outer - inner, 0), outer);
        }
    }
    return integrated * (1.0 / 16.0);
}
