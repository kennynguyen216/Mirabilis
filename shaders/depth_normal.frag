#version 450

layout(location = 0) in vec3 inViewNormal;

layout(location = 0) out vec4 outNormal;

void main()
{
    // Stored unencoded while the buffer is being brought up; it is easy to
    // read in the debug views.  Octahedral encoding into two channels is the
    // obvious later saving.
    outNormal = vec4(normalize(inViewNormal), 1.0);
}
