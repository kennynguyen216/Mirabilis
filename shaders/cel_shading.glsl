// Brightness bands retain hue and HDR range. Square-root spacing gives dim interiors more bands.
vec3 cel_bands(vec3 color,float bands)
{
    float brightness=dot(max(color,vec3(0)),vec3(0.2126,0.7152,0.0722));
    if(brightness<1e-5)return vec3(0);
    float band=floor(sqrt(brightness)*max(bands,2.0)+0.5)/max(bands,2.0);
    return color*(band*band/brightness);
}
