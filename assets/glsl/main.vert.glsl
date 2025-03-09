#version 330 core

// attributes and uniforms are provided to the shader by the external program
// attributes are per-vertex, taken from the VBO or vertex array data
// uniforms are set once, and remain constant for a given draw call

// layout must match order of attributes in the combined buffer
// as we calculate the offset between between each field based
// on their positions
// layout (...) must be on its own line for our introspection to work
layout(location = 0)
in vec3 position;
layout(location = 1)
in uint fgcolor;
layout(location = 2)
in uint bgcolor;
layout(location = 3)
in vec2 texcoord;
layout(location = 4)
in vec2 texBox;
layout(location = 5)
in uint layer;
layout(location = 6)
in uvec2 tag;

// mvp == projection view model matrix
// model translates model local coordinates to world space
//  vertex buffer object data is always in model local coordinates
//  where (0, 0, 0) is the center of the model
// view translates the world around the "camera"
// perspective projection translates the view, applying depth illusion
// orthographic projection quashes the z in the view
//  each can independently contain translation, rotation, and scale
//  which must be applied in that order (trans * rot * scale * vector position)

uniform mat4 projection;
uniform mat4 view;
uniform mat4 model;

// values are variables internal to the vertex and fragment shaders
// they are set in the vertex shader, and their interpolated values across
// the three vertexes of every triangle are used to construct the color value
// in the fragment shader for every "fragment" of color that makes up the
// triangle

out Vertex {
    uint fgcolor;
    uint bgcolor;
    vec2 texcoord;
    vec2 texBox;
    uint layer;
    uvec2 tag;
} v;

void main()
{
    v.fgcolor = fgcolor;
    v.bgcolor = bgcolor;
    v.texcoord = texcoord;
    v.texBox = texBox;
    v.layer = layer;
    v.tag = tag;
    // the first three values of the position vector are the familiar (x,y,z)
    // the fourth is w, when w == 1.0, it indicates that the x, y, and z refer to
    // a position, when w == 0.0, it indicates that they refer to a *direction*,
    // which is hardly ever useful
    gl_Position = projection * view * model * vec4(position, 1);
}
