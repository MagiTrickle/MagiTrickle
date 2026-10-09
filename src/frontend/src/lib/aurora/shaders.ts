// WebGL 1 keeps the effect available on older WebViews. All noise is continuous
// in time; there is no repeating timeline or precomputed wave silhouette.
export const vertexShader = `
attribute vec2 position;
varying vec2 uv;
void main() {
  uv = position * 0.5 + 0.5;
  gl_Position = vec4(position, 0.0, 1.0);
}`;

export const atmosphereShader = `
precision highp float;
varying vec2 uv;
uniform vec2 size;
uniform vec2 anchor;
uniform float time;

float hash(vec2 p) {
  vec3 p3 = fract(vec3(p.xyx) * .1031);
  p3 += dot(p3, p3.yzx + 33.33);
  return fract((p3.x + p3.y) * p3.z);
}
float noise(vec2 p) {
  vec2 i = floor(p), f = fract(p);
  f = f*f*f*(f*(f*6.0-15.0)+10.0);
  return mix(mix(hash(i), hash(i+vec2(1,0)), f.x),
             mix(hash(i+vec2(0,1)), hash(i+vec2(1,1)), f.x), f.y);
}
float fbm(vec2 p) {
  float n = .5 * noise(p);
  p = mat2(.8, -.6, .6, .8) * p * 2.03 + 13.7;
  n += .21 * noise(p);
  p = mat2(.8, -.6, .6, .8) * p * 2.01 + 7.3;
  return n + .09 * noise(p);
}
void main() {
  float scale = min(size.y, max(620.0, size.x * 1.4));
  // Top-down coordinates, shared with the DOM logo's measured position.
  vec2 p = (vec2(uv.x, 1.0-uv.y) * size - anchor) / scale;
  float t = time * .022;
  vec2 drift = vec2(t * .23, -t * .17);
  float cloud = fbm(p * 2.1 + drift);
  vec3 col = vec3(.006, .012, .027);
  col += vec3(.008, .025, .055) * exp(-dot(p*vec2(.8,1.3),p*vec2(.8,1.3))*1.8);
  col += vec3(.012, .025, .055) * cloud * cloud;

  // Three sheets occupy different depths. Domain warping changes their folds,
  // density and curvature, while broad scattering softens the fine filaments.
  for (int i = 0; i < 3; i++) {
    float layer = float(i);
    vec2 q = p;
    if (i == 2) q = mat2(.94,-.34,.34,.94)*p + vec2(.1,-.33);
    q.x += .1 * layer;
    q.y += .035 * layer;
    float crest = .31 - .38 * exp(-q.x*q.x*7.5) + .18*q.x;
    q.y -= crest;
    vec2 flow = vec2(q.x * 1.25 - t*(.14+layer*.07), q.y*1.7);
    float warp = fbm(flow + vec2(layer*7.1, t*.12));
    float folds = fbm(flow*1.7 + vec2(warp*1.7, layer*3.7-t*.1));
    float d = q.y + (warp-.40)*.46 + (folds-.40)*.12;
    float envelope = exp(-d*d * (30.0 + layer*12.0));
    float detail = noise(vec2(flow.x*3.0 + t*.2, d*12.0 + folds*3.0));
    float pleats = pow(.5+.5*sin(d*68.0 + folds*14.0 + warp*9.0),14.0);
    float veil = envelope * (.035 + .40*folds*folds + pleats*.11*detail);
    float edge = exp(-abs(d + .018 + folds*.02)*78.0) * (.3+detail*.7);
    float scattering = exp(-d*d*13.0)*.06;
    float illumination = .48 + .52*noise(vec2(q.x*2.8-t*.32, layer*5.0));
    float fade = exp(-abs(p.x)*.55) * (1.0-layer*.19);
    vec3 blue = vec3(.055, .24, .70);
    vec3 cyan = vec3(.12, .73, .82);
    vec3 tint = mix(blue, cyan, smoothstep(.28,.68,folds + p.x*.14));
    tint = mix(tint, vec3(.27,.19,.59), .14*smoothstep(.2,.8,-p.x));
    col += tint * (veil + edge*.32 + scattering) * illumination * fade * 1.8;
  }

  // A broken, depth-occluded eddy curves into the mark. Its density comes
  // from the surrounding flow, so it reads as a wisp rather than an orbit UI.
  vec2 eddy = mat2(.92,-.39,.39,.92) * (p + vec2(.005,-.005));
  float radius = length(eddy*vec2(.72,1.8));
  float eddyNoise = fbm(eddy*5.0 + vec2(t*.08,3.0));
  float eddyDistance = radius - .162 + (eddyNoise-.4)*.045;
  float broken = smoothstep(.25,.65,noise(eddy*8.0+vec2(t*.12,8.0)));
  col += vec3(.12,.55,.82) * exp(-abs(eddyDistance)*210.0)*broken*.22;
  col += vec3(.025,.12,.20) * exp(-eddyDistance*eddyDistance*900.0)*broken*.2;

  // Distant dust and a soft reflected pool give the scene scale without a grid
  // or a literal horizon. Stars stay sparse and vary gently, never flashing.
  float mist = fbm(p*vec2(2.2,4.8) + vec2(9.0, -t*.09));
  col += vec3(.016,.055,.10) * mist * exp(-pow((p.y-.62)*5.0,2.0));
  vec2 starCoord = (vec2(uv.x,1.0-uv.y)*size)/4.0;
  vec2 cell = floor(starCoord);
  float seed = hash(cell);
  float star = pow(max(0.0,1.0-length(fract(starCoord)-.5)*2.0), 7.0);
  star *= step(.997, seed) * (.22+.10*sin(time*.23 + seed*312.0));
  col += vec3(.48,.70,1.0)*star;
  float vignette = 1.0 - .4 * smoothstep(.25, 1.0, length((uv-.5)*vec2(1.0,.85)));
  col *= vignette;
  // Dither below a display code value to prevent bands in dark gradients.
  col += (hash(gl_FragCoord.xy)-.5)/255.0;
  gl_FragColor = vec4(col, 1.0);
}`;

export const compositeShader = `
precision highp float;
varying vec2 uv;
uniform sampler2D atmosphere;
uniform sampler2D logo;
uniform vec2 size;
uniform vec4 logoRect;
void main() {
  vec3 space = texture2D(atmosphere,uv).rgb;
  vec3 col = space;
  vec2 pixel = vec2(uv.x,1.0-uv.y)*size;
  vec2 local = (pixel-logoRect.xy)/logoRect.zw;
  // Only silhouette scattering lives in this pass. The browser renders the
  // original SVG above it, at native display resolution and browser zoom.
  if (local.x > -.08 && local.y > -.08 && local.x < 1.08 && local.y < 1.08) {
    float coverage = texture2D(logo,clamp(local,0.0,1.0)).a;
    vec2 offset = logoRect.zw/size*.09;
    vec3 light = (space*2.0 + texture2D(atmosphere,uv+offset).rgb + texture2D(atmosphere,uv-offset).rgb)*.25;
    float energy = dot(light,vec3(.2,.6,.2));
    float surround = 0.0;
    for (int i=0;i<8;i++) {
      float a = float(i)*.785398;
      surround += texture2D(logo,clamp(local+vec2(cos(a),sin(a))*.028,0.0,1.0)).a;
    }
    // Reach zero before the branch boundary, including at coarse resolutions.
    // A clamped alpha sample must never expose the rectangular texture bounds.
    float boundary = max(abs(local.x-.5),abs(local.y-.5));
    float fade = 1.0-smoothstep(.50,.57,boundary);
    col += vec3(.12,.44,.65) * surround*.125*(1.0-coverage)*(.055+energy*.5)*fade;
  }
  gl_FragColor = vec4(col,1.0);
}`;
