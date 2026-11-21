#version 450
// The window's picture: one triangle over the viewport (the picture's place
// in the window), with the frame's texture coordinates 0..1 across it.
layout(location = 0) out vec2 uv;

void main() {
    const vec2 corner = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);  // (0,0) (2,0) (0,2)
    uv = corner;
    gl_Position = vec4(corner * 2.0 - 1.0, 0.0, 1.0);
}
