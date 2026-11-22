#version 450
// SPDX-License-Identifier: ISC
// Copyright 2026 nx-hbmenu Switch 2 port contributors
// Texel times vertex colour; blending is fixed function.

layout(set = 0, binding = 0) uniform sampler2D image;

layout(location = 0) in vec2 v_texcoord;
layout(location = 1) in vec4 v_color;

layout(location = 0) out vec4 out_color;

void main() {
    out_color = texture(image, v_texcoord) * v_color;
}
