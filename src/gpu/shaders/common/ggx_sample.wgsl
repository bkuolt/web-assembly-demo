// Shared low-discrepancy Hammersley & Van der Corput sampling helpers
#pragma once

fn radicalInverseVdC(bitsIn : u32) -> f32 {
    var bits = bitsIn;
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return f32(bits) * 2.3283064365386963e-10;
}

fn hammersley(i : u32, N : u32) -> vec2<f32> {
    return vec2<f32>(f32(i) / f32(N), radicalInverseVdC(i));
}

