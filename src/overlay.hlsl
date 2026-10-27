// Text-free microphone field on an Orchard floating surface. Host owns
// envelopes, audio phase, lifetime and every colour.
cbuffer Parameters : register(b0) {
    float4 viewport; // width, height, DPI scale, audio phase
    float4 shape;    // corner radius (logical px)
    float4 fill;     // elevated surface
    float4 border;   // hairline
    float4 ring;     // accent: dots while speaking
    float4 sound;
    float4 bands0;
    float4 bands1;
    float4 quiet;    // muted: dots at rest
};

float roundedDistance(float2 p, float2 halfSize, float radius) {
    float2 q = abs(p) - halfSize + radius;
    return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - radius;
}

float spectralBand(int index) {
    index = clamp(index, 0, 7);
    if (index < 4) return saturate(bands0[index]);
    return saturate(bands1[index - 4]);
}

float4 main(float4 pixel : SV_POSITION, float2 uv : TEXCOORD0) : SV_TARGET {
    pixel.xy = uv * viewport.xy;
    const float pi = 3.14159265359;
    float dpi = max(viewport.z, 0.25);
    float pixelWidth = 1.0 / dpi;
    float2 p = (pixel.xy - viewport.xy * 0.5) / dpi;
    float2 halfSize = viewport.xy * (0.5 / dpi) - 0.5 * pixelWidth;
    float radius = min(shape.x, min(halfSize.x, halfSize.y));
    float edge = roundedDistance(p, halfSize, radius);
    float edgeAA = max(fwidth(edge), pixelWidth);
    float alpha = 1.0 - smoothstep(-0.5 * edgeAA, 0.5 * edgeAA, edge);

    // Flat face with a one-device-pixel hairline; no gradient, no shadow.
    float hairline = saturate((edge + pixelWidth) / edgeAA + 0.5);
    float3 color = lerp(fill.rgb, border.rgb, hairline);

    float energy = saturate(sound.x);
    float expansion = sqrt(energy);
    bool finishing = sound.z > 0.5;
    bool reduced = sound.w > 0.5;
    float pitch = saturate(sound.y);
    float phase = viewport.w; // Continuous host phase advances only with audio energy.
    float low = dot(saturate(bands0), float4(0.4, 0.3, 0.2, 0.1));
    float high = dot(saturate(bands1), float4(0.1, 0.2, 0.3, 0.4));

    // Persistent three-dot constellations open from a 48px row into an airy field.
    // Audio phase advects them; local spectrum opens each orbit, pitch twists it.
    // All displacement vanishes with energy, regardless of the final phase.
    float spacing = lerp(4.0, 11.5, expansion);
    if (reduced) spacing = 8.5;
    if (finishing) spacing = 6.0;
    spacing = min(spacing, max(halfSize.x - 16.0, 1.0) / 6.0);
    float nearestColumn = floor(p.x / spacing + 0.5);
    float dotCoverage = 0.0;
    float3 dotLight = 0.0;

    // Only neighboring columns can cover this pixel: nine circle evaluations,
    // independent of viewport size. No full particle scan or texture sampling.
    [unroll]
    for (int neighbor = -1; neighbor <= 1; ++neighbor) {
        float column = nearestColumn + neighbor;
        float valid = saturate(7.0 - abs(column));
        float u = clamp(column / 6.0, -1.0, 1.0);
        float bandPosition = (u * 0.5 + 0.5) * 7.0;
        int bandIndex = (int)floor(bandPosition);
        float band = lerp(spectralBand(bandIndex), spectralBand(bandIndex + 1), frac(bandPosition));
        float taper = sqrt(saturate(1.0 - 0.55 * u * u));
        float centerY = expansion * (2.2 * sin(phase * 0.73 - u * pi * (1.0 + pitch))
                                   + 1.3 * (low - high) * sin(phase * 0.41 + u * 2.0 * pi));
        float orbitHeight = expansion * (6.5 + 5.5 * sqrt(band)) * taper;
        // Rates depend on persistent column identity, not changing audio values:
        // changing a band cannot jump an accumulated phase to a new clock.
        float orbitPhase = phase * (0.85 + 0.035 * (column + 6.0))
                         + u * pi * (0.8 + 0.6 * pitch) + 0.8 * band;

        [unroll]
        for (int row = -1; row <= 1; ++row) {
            float visibility = row == 0 ? 1.0 : smoothstep(0.035, 0.4, expansion);
            float dotRadius = 1.0 + 0.55 * expansion + 0.22 * band * energy;
            float brightness = lerp(0.6, 1.0, expansion);
            if (row != 0) { dotRadius *= 0.8; brightness *= 0.82; }
            float angle = orbitPhase + row * (2.0 * pi / 3.0);
            float2 center = float2(column * spacing + 0.24 * spacing * expansion * cos(angle),
                                   centerY + orbitHeight * sin(angle));
            if (reduced) {
                // Neither spectrum, phase nor energy can move reduced-motion centers.
                center = float2(column * spacing, row * 9.0 * taper);
            }

            if (finishing) {
                // Three stationary five-dot diamonds; not a progress indicator.
                float groupOffset = abs(column - 4.0 * floor(column / 4.0 + 0.5));
                visibility = row == 0 ? 1.0 - step(1.5, groupOffset)
                                      : 1.0 - step(0.5, groupOffset);
                dotRadius = row == 0 ? 1.3 : 1.1;
                brightness = 1.0;
                center = float2(column * spacing, row * 5.0);
            }
            float distanceToDot = length(p - center) - dotRadius;
            float coverage = (1.0 - smoothstep(-0.5 * pixelWidth, 0.5 * pixelWidth, distanceToDot))
                             * visibility * valid;
            float3 tone = lerp(quiet.rgb, ring.rgb, finishing ? 0.0 : smoothstep(0.0, 0.35, expansion));
            dotCoverage = max(dotCoverage, coverage);
            dotLight = max(dotLight, tone * brightness * coverage);
        }
    }
    // Keep every dot inside the matte face; premultiply after all composition.
    float inset = 1.0 - smoothstep(-3.0, -2.0, edge);
    color = color * (1.0 - dotCoverage * inset) + dotLight * inset;
    return float4(color * alpha, alpha);
}
