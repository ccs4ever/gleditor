#version 330 core

layout(points) in;
layout(triangle_strip, max_vertices = 14) out;

uniform mat4 model;
uniform float cubeDepth;

in Vertex {
    uint fgcolor;
    uint bgcolor;
    vec2 texcoord;
    vec2 texBox;
    uint layer;
    ivec2 tag;
} v[];

out Frag {
    vec3 fgcolor;
    vec3 bgcolor;
    vec2 texcoord;
    float layer;
    flat ivec2 tag;
} f;

// GL 4.1 provies unpackUNorm for this purpose
vec3 c(uint i) {
    return vec3(uint(i >> 24) / 255.0,
        (uint(i >> 16) & uint(255)) / 255.0,
        (uint(i >> 8) & uint(255)) / 255.0);
}

vec3 lwh(uint i) {
    return vec3(i >> uint(28), (i >> uint(14)) & uint(16383), i & uint(16383));
}

vec4 addPos(vec4 fin) {
    return gl_in[0].gl_Position + (model * fin);
}

void main() {
    vec3 fgcolor = c(v[0].fgcolor);
    vec3 bgcolor = c(v[0].bgcolor);
    vec3 l = lwh(v[0].layer);

    if (0 == l.y || 0 == l.z) {
        return;
    }

    f.layer = l.x;
    float width = l.y / 2.0;
    float height = l.z / 2.0;
    //f.fgcolor = vec3(0, 0, 1);
    //f.bgcolor = vec3(0, 0, 1);

    if (0 != cubeDepth) {
        vec3 foo[14] = vec3[](
                vec3(-width, height, cubeDepth), // Front-top-left
                vec3(width, height, cubeDepth), // Front-top-right
                vec3(-width, -height, cubeDepth), // Front-bottom-left
                vec3(width, -height, cubeDepth), // Front-bottom-right
                vec3(width, -height, -cubeDepth), // Back-bottom-right
                vec3(width, height, cubeDepth), // Front-top-right
                vec3(width, height, -cubeDepth), // Back-top-right
                vec3(-width, height, cubeDepth), // Front-top-left
                vec3(-width, height, -cubeDepth), // Back-top-left
                vec3(-width, -height, cubeDepth), // Front-bottom-left
                vec3(-width, -height, -cubeDepth), // Back-bottom-left
                vec3(width, -height, -cubeDepth), // Back-bottom-right
                vec3(-width, height, -cubeDepth), // Back-top-left
                vec3(width, height, -cubeDepth) // Back-top-right
            );

        for (int i = 0; i < 14; i++) {
            f.fgcolor = fgcolor;
            f.bgcolor = bgcolor;
            f.tag = v[0].tag;
            gl_Position = addPos(vec4(foo[i], 0));
            //f.texcoord = vec2(foo[i].xy / (i+1));
            //f.fgcolor = mix(vec3(1,0,0),vec3(0,0,1), i%2);
            EmitVertex();
        }
    } else {
        const float z = 0;
        // top left {0, 1}
        f.fgcolor = fgcolor;
        f.bgcolor = bgcolor;
        f.tag = v[0].tag;
        f.texcoord = v[0].texcoord + vec2(0, v[0].texBox.y);
        gl_Position = addPos(vec4(-width, height, z, 0));
        EmitVertex();

        // top right
        f.fgcolor = fgcolor;
        f.bgcolor = bgcolor;
	f.tag = v[0].tag;
        f.texcoord = v[0].texcoord + v[0].texBox;
        gl_Position = addPos(vec4(width, height, z, 0));
        EmitVertex();

        // bottom left
        f.fgcolor = fgcolor;
        f.bgcolor = bgcolor;
	f.tag = v[0].tag;
        f.texcoord = v[0].texcoord;
        gl_Position = addPos(vec4(-width, -height, z, 0));
        EmitVertex();

        // bottom right
        f.fgcolor = fgcolor;
        f.bgcolor = bgcolor;
	f.tag = v[0].tag;
        f.texcoord = v[0].texcoord + vec2(v[0].texBox.x, 0);
        gl_Position = addPos(vec4(width, -height, z, 0));
        EmitVertex();
    }

    EndPrimitive();
}
