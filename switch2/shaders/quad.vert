#version 450
// SPDX-License-Identifier: ISC
// Copyright 2026 nx-hbmenu Switch 2 port contributors
// Quads in physical pixels; push.scale maps them to clip space.

layout(push_constant) uniform Push { vec2 scale; } push;

layout(location = 0) in vec2 position;
layout(location = 1) in vec2 texcoord;
layout(location = 2) in vec4 color;

layout(location = 0) out vec2 v_texcoord;
layout(location = 1) out vec4 v_color;

void main() {
    v_texcoord = texcoord;
    v_color = color;
    gl_Position = vec4(position * push.scale - 1.0, 0.0, 1.0);
}
