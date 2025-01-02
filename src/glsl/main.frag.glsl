#version 330

// version 130 == opengl 3.0
// the texture unit that we will pull texture data from, starts at 0
// there are a limited number of units and thus a limited number of
// textures that can be active at the same time, 2d texture arrays
// and other such things can be used to pack more textures into a
// single unit, check total number with 
// glGetIntegerv(GL_MAX_TEXTURE_IMAGE_UNITS, &v)

uniform sampler2DArray texUnit;

in vec3 v_fgcolor;
in vec3 v_bgcolor;
in vec2 v_texcoord;
in float v_layer;

void main()
{
		//gl_FragColor = vec4(1,0,0,1);
		gl_FragColor = vec4(
				mix(v_fgcolor, v_bgcolor, texture(texUnit, 
						vec3(v_texcoord, floor(v_layer+0.5))).r), 1);
}
