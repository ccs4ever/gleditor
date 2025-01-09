#version 330 core

layout (points) in;
layout (triangle_strip, max_vertices = 14) out;

uniform mat4 model;
uniform float cubeDepth;


in uint v_fgcolor[];
in uint v_bgcolor[];
in vec2 v_texcoord[];
in uint v_layer[];

out vec3 f_fgcolor;
out vec3 f_bgcolor;
out vec2 f_texcoord;
out float f_layer;

// GL 4.1 provies unpackUNorm for this purpose
vec3 c(uint i) { 
    return vec3(uint(i >> 24) / 255.0, 
                (uint(i >> 16) & uint(255)) / 255.0, 
                (uint(i >> 8) & uint(255)) / 255.0);
}

vec3 lwh(uint i) {
    return vec3(i >> uint(24), uint(i >> 12) & uint(4095), i & uint(4095));

}

vec4 f(vec4 fin) {
return gl_in[0].gl_Position + vec4((model * fin).rgb, 0);
}

void drawFace(float z, float width, float height) {
    // top left {0, 1}
    f_texcoord = v_texcoord[0];
    gl_Position = vec4(gl_in[0].gl_Position.xy, f(vec4(0,0,z,0)).z, gl_in[0].gl_Position.w);
    EmitVertex();
    
    // top right
    f_texcoord = vec2(1, 2) - v_texcoord[0];
    gl_Position = f(vec4(width, 0, z, 0));
    EmitVertex();
    
    // bottom left
    f_texcoord = vec2(0, 1) - v_texcoord[0];
    gl_Position = f(vec4(0, -height, z, 1));
    EmitVertex();
    
    // bottom right
    f_texcoord = vec2(1, 1) - v_texcoord[0];
    gl_Position = f(vec4(width, -height, z, 0));
    EmitVertex();
    

}

void main() {
    f_fgcolor = c(v_fgcolor[0]);
    f_bgcolor = c(v_bgcolor[0]);
    vec3 l = lwh(v_layer[0]);
    f_layer = l.x;
    float width = l.y/2.0;
    float height = l.z/2.0;
    //f_fgcolor = vec3(0, 0, 1);
    //f_bgcolor = vec3(0, 0, 1);

if (0 != cubeDepth) {
    //f_texcoord = v_texcoord[0];
    width = width/2; height = height/2;
vec3 foo[14] = vec3[](
    vec3(-width, height, cubeDepth),     // Front-top-left
    vec3(width, height, cubeDepth),      // Front-top-right
    vec3(-width, -height, cubeDepth),    // Front-bottom-left
    vec3(width, -height, cubeDepth),     // Front-bottom-right
    vec3(width, -height, -cubeDepth),    // Back-bottom-right
    vec3(width, height, cubeDepth),      // Front-top-right
    vec3(width, height, -cubeDepth),     // Back-top-right
    vec3(-width, height, cubeDepth),     // Front-top-left
    vec3(-width, height, -cubeDepth),    // Back-top-left
    vec3(-width, -height, cubeDepth),    // Front-bottom-left
    vec3(-width, -height, -cubeDepth),   // Back-bottom-left
    vec3(width, -height, -cubeDepth),    // Back-bottom-right
    vec3(-width, height, -cubeDepth),    // Back-top-left
    vec3(width, height, -cubeDepth)      // Back-top-right
);

for (int i = 0; i < 14; i++) {
	gl_Position = f(vec4(foo[i],0));
    //f_texcoord = vec2(foo[i].xy / (i+1));
//f_fgcolor = mix(vec3(1,0,0),vec3(0,0,1), i%2);
EmitVertex();
}
} else {
    //drawFace(0, width, height);
    float z = 0;
    // top left {0, 1}
    f_texcoord = v_texcoord[0];
    gl_Position = f(vec4(-width, height, z, 0));
    EmitVertex();
    
    // top right
    f_texcoord = vec2(1, 2) - v_texcoord[0];
    gl_Position = f(vec4(width, height, z, 0));
    EmitVertex();
    
    // bottom left
    f_texcoord = vec2(0, 1) - v_texcoord[0];
    gl_Position = f(vec4(-width, -height, z, 1));
    EmitVertex();
    
    // bottom right
    f_texcoord = vec2(1, 1) - v_texcoord[0];
    gl_Position = f(vec4(width, -height, z, 0));
    EmitVertex();
}

    EndPrimitive();

}
