// Flat Orchard surface: one rounded rectangle with a hairline border and an
// optional inner focus ring. Device pixels; output premultiplied. The fill may
// be a translucent frosted layer; border and ring stay opaque.
cbuffer Parameters : register(b0) {
    float4 viewport; // width, height
    float4 shape;    // corner radius, border width, ring width, opacity
    float4 fill;     // rgb, alpha
    float4 border;
    float4 ring;
};

float distanceToShape(float2 p, float2 halfSize, float radius) {
    float2 q = abs(p) - halfSize + radius;
    return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - radius;
}

// 1 within `width` of the outer edge, 0 further inside, one-pixel antialiased.
float edgeBand(float distance, float width) {
    return width > 0 ? saturate(distance + width + 0.5) : 0;
}

float4 main(float4 pixel : SV_POSITION, float2 uv : TEXCOORD0) : SV_TARGET {
    float2 halfSize = viewport.xy * 0.5;
    float2 p = uv * viewport.xy - halfSize;
    float radius = min(shape.x, min(halfSize.x, halfSize.y));
    float d = distanceToShape(p, halfSize, radius);
    float coverage = saturate(0.5 - d) * shape.w;
    float4 color = float4(fill.rgb * fill.a, fill.a);
    color = lerp(color, float4(border.rgb, 1), edgeBand(d, shape.y));
    color = lerp(color, float4(ring.rgb, 1), edgeBand(d, shape.z));
    return color * coverage;
}
