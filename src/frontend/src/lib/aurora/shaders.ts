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
// Jittered cells avoid a visible grid. Each depth has its own scale, softness
// and drift; the distant stars remain stationary while foreground dust moves.
float stars(vec2 pixel, float spacing, float threshold, float radius) {
  vec2 cell = floor(pixel/spacing);
  vec2 center = vec2(hash(cell+17.3),hash(cell+41.8))*.6+.2;
  vec2 delta = (fract(pixel/spacing)-center)*spacing;
  float seed = hash(cell+9.1);
  float core = exp(-dot(delta,delta)/(radius*radius));
  float bloom = exp(-dot(delta,delta)/(radius*radius*12.0))*.045;
  float shimmer = .83+.17*sin(time*.12+seed*129.0);
  return (core+bloom)*step(threshold,seed)*(.35+.65*seed)*shimmer;
}
void main() {
  float scale = min(size.y, max(620.0, size.x * 1.4));
  // Top-down coordinates, shared with the DOM logo's measured position.
  vec2 p = (vec2(uv.x, 1.0-uv.y) * size - anchor) / scale;
  float t = time * .022;
  vec2 drift = vec2(t * .23, -t * .17);
  vec3 col = vec3(.003, .006, .015);

  // Broad, irregular clouds sit behind the silk, with cooler emission in the
  // depths and a little violet in the distant dust. Dark lanes absorb light
  // instead of filling every empty region with an additive blue wash.
  vec2 nebula = mat2(.91,-.41,.41,.91)*(p-vec2(.12,.22));
  vec2 warp = vec2(fbm(nebula*1.7+drift*.3),fbm(nebula*1.9+vec2(8.1,2.4)-drift*.2));
  float cloud = fbm(nebula*4.3+warp*2.6+drift*.14);
  float wisps = fbm(nebula*10.0+warp*3.1-drift*.21);
  float volume = exp(-nebula.y*nebula.y*2.7-nebula.x*nebula.x*.55);
  float density = smoothstep(.23,.63,cloud)*volume;
  float dustLane = smoothstep(.38,.65,fbm(nebula*3.1-warp*1.6+11.0));
  float cavity = exp(-dot((p-vec2(.32,-.18))*vec2(1.8,2.6),(p-vec2(.32,-.18))*vec2(1.8,2.6)));
  vec3 gas = mix(vec3(.066,.047,.15),vec3(.028,.17,.23),smoothstep(.26,.59,warp.x));
  col += gas*density*(.36+wisps*.85)*(1.0-dustLane*.7)*(1.0-cavity*.52);
  col += vec3(.018,.056,.105)*pow(density,2.0)*(1.0-dustLane)*1.2;
  col += vec3(.009,.025,.048)*exp(-dot(p*vec2(.9,1.4),p*vec2(.9,1.4))*2.0);

  vec2 sky = vec2(uv.x,1.0-uv.y)*size;
  float extinction = 1.0-density*.62;
  col += vec3(.48,.61,.83)*stars(sky,29.0,.73,.65)*.19*extinction;
  col += vec3(.67,.81,1.0)*stars(sky+153.7,67.0,.79,1.05)*.43*extinction;

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
    col += tint * (veil + edge*.24 + scattering) * illumination * fade * 1.48;
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

  // A separate foreground veil partially obscures the silk. Its slow drift
  // creates depth without camera motion or particles competing with the form.
  float fog = fbm(p*vec2(3.1,2.0)+vec2(13.0,7.0)+drift*.43);
  float foreground = smoothstep(.3,.68,fog)*exp(-pow((p.y-.58)*1.5,2.0));
  col *= 1.0-foreground*.26-dustLane*volume*.14;
  col += vec3(.016,.040,.065)*foreground*.55;
  vec2 particles = sky + vec2(time*1.15,-time*.65);
  col += vec3(.30,.57,.70)*stars(particles+57.0,137.0,.91,1.7)*.23;
  float vignette = 1.0-.55*smoothstep(.18,.72,length((uv-.5)*vec2(.95,.85)));
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
      surround += texture2D(logo,clamp(local+vec2(cos(a),sin(a))*.021,0.0,1.0)).a;
    }
    // Reach zero before the branch boundary, including at coarse resolutions.
    // A clamped alpha sample must never expose the rectangular texture bounds.
    float boundary = max(abs(local.x-.5),abs(local.y-.5));
    float fade = 1.0-smoothstep(.50,.57,boundary);
    col += vec3(.12,.44,.65) * surround*.125*(1.0-coverage)*(.028+energy*.28)*fade;
  }
  gl_FragColor = vec4(col,1.0);
}`;
