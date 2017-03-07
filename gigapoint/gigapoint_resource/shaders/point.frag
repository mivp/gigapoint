varying vec3 vColor;

void main() {
  
#if defined SQUARE_POINT_SHAPE
    gl_FragColor = vec4(vColor,1.0);
    return;
#endif
    gl_FragColor = vec4(vColor,1.0);
    vec3 N;
    N.xy = gl_PointCoord* 2.0 - vec2(1.0);    
    float mag = dot(N.xy, N.xy);
    if (mag > 1.0) {
        discard;
    }
}