#version 330 core

layout (points) in;
layout (triangle_strip, max_vertices = 4) out;

in uint v_fgcolor[];
in uint v_bgcolor[];
in vec2 v_texcoord[];
in float v_layer[];

out vec3 f_fgcolor;
out vec3 f_bgcolor;
out vec2 f_texcoord;
out float f_layer;

vec3 c(uint i) { 
    return vec3(uint(i >> 24) / 255.0, 
                (uint(i >> 16) & uint(255)) / 255.0, 
                (uint(i >> 8) & uint(255)) / 255.0);
}

void main() {
    f_fgcolor = c(v_fgcolor[0]);
    f_bgcolor = c(v_bgcolor[0]);
    f_texcoord = v_texcoord[0];
    f_layer = v_layer[0];


    // bottom left
    gl_Position = gl_in[0].gl_Position + vec4(0, -30, 0, 0);
    EmitVertex();
    // bottom right
    gl_Position = gl_in[0].gl_Position + vec4(30, -30, 0, 0);
    EmitVertex();
    // top left
    gl_Position = gl_in[0].gl_Position;
    EmitVertex();
    // top right
    gl_Position = gl_in[0].gl_Position + vec4(30, 0, 0, 0);
    EmitVertex();

    EndPrimitive();

}
