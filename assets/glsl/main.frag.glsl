#version 330 core

// version 130 == opengl 3.0
// the texture unit that we will pull texture data from, starts at 0
// there are a limited number of units and thus a limited number of
// textures that can be active at the same time, 2d texture arrays
// and other such things can be used to pack more textures into a
// single unit, check total number with 
// glGetIntegerv(GL_MAX_TEXTURE_IMAGE_UNITS, &v)

uniform sampler2DArray texUnit;

in vec3 f_fgcolor;
in vec3 f_bgcolor;
in vec2 f_texcoord;
in float f_layer;

out vec4 outColor;

void main()
{
		//gl_FragColor = vec4(1,0,0,1);
		outColor = vec4(
				mix(f_fgcolor, f_bgcolor, texture(texUnit, 
						vec3(f_texcoord, floor(f_layer+0.5))).r), 1);
}
