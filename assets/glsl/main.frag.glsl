#version 330 core

// version 130 == opengl 3.0
// the texture unit that we will pull texture data from, starts at 0
// there are a limited number of units and thus a limited number of
// textures that can be active at the same time, 2d texture arrays
// and other such things can be used to pack more textures into a
// single unit, check total number with
// glGetIntegerv(GL_MAX_TEXTURE_IMAGE_UNITS, &v)

uniform sampler2DArray texGlyphCache;
uniform float cubeDepth;

in Frag {
    vec3 fgcolor;
    vec3 bgcolor;
    vec2 texcoord;
    float layer;
    flat uvec2 tag;
} f;

layout(location = 0)
out vec4 outColor;
layout(location = 1)
out uvec2 tag;

void main()
{
    //gl_FragColor = vec4(1,0,0,1);
    if (0 != cubeDepth) {
        outColor = vec4(f.bgcolor, 1);
    } else {
        float bgFgRatio = texture(texGlyphCache,
                vec3(f.texcoord, floor(f.layer + 0.5))).r;
        outColor = vec4(
                mix(f.bgcolor, f.fgcolor, bgFgRatio
                ), 1);
    }
    tag = f.tag;
}
