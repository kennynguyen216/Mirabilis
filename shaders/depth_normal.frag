#version 450

layout(location = 0) in vec3 inViewNormal;
layout(location=2) flat in float inDoubleSided;
layout(location=3) in vec3 inViewPosition;
layout(location=4) flat in float inStaticCacheMember;

layout(location = 0) out vec4 outNormal;

void main()
{
    // Stored unencoded while the buffer is being brought up; it is easy to
    // read in the debug views.  Octahedral encoding into two channels is the
    // obvious later saving.
    vec3 normal=normalize(inViewNormal);
    if(inDoubleSided>0.5 && dot(normal,-inViewPosition)<0.0)normal=-normal;
    outNormal=vec4(normal,inStaticCacheMember);
}
