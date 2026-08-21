#version 450

layout(location = 0) out vec4 outFragColor;

void main()
{
    // Bright cyan stays distinct from the blue/orange portal colours.
    outFragColor = vec4(0.0, 1.0, 0.85, 1.0);
}
