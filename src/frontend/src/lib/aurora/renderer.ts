import { atmosphereShader, compositeShader, vertexShader } from "./shaders";

export type AuroraRenderer = {
  resize: (width: number, height: number, logo: DOMRect, scene: DOMRect) => void;
  draw: (seconds: number) => void;
  reduceQuality: () => boolean;
  destroy: () => void;
};

/** Two passes: a scalable light field, then silhouette scattering beneath the SVG. */
export function createAuroraRenderer(
  canvas: HTMLCanvasElement,
  image: HTMLImageElement,
): AuroraRenderer | undefined {
  let context: WebGLRenderingContext | null;
  try {
    context = canvas.getContext("webgl", {
      alpha: false,
      antialias: false,
      depth: false,
      stencil: false,
      powerPreference: "low-power",
      preserveDrawingBuffer: false,
    });
  } catch {
    return;
  }
  const gl = context;
  if (!gl) return;

  const shaders: WebGLShader[] = [];
  const programs: WebGLProgram[] = [];
  const textures: WebGLTexture[] = [];
  let buffer: WebGLBuffer | null = null;
  let framebuffer: WebGLFramebuffer | null = null;
  const destroy = () => {
    shaders.forEach((shader) => gl.deleteShader(shader));
    programs.forEach((program) => gl.deleteProgram(program));
    textures.forEach((texture) => gl.deleteTexture(texture));
    gl.deleteBuffer(buffer);
    gl.deleteFramebuffer(framebuffer);
  };

  try {
    function compile(source: string, type: number) {
      const shader = gl!.createShader(type);
      if (!shader) throw new Error("Shader unavailable");
      shaders.push(shader);
      // Fragment highp is optional on WebGL 1. Fall back instead of drawing
      // unstable noise on a GPU whose fragment precision is insufficient.
      gl!.shaderSource(shader, source);
      gl!.compileShader(shader);
      if (!gl!.getShaderParameter(shader, gl!.COMPILE_STATUS)) {
        throw new Error("Aurora shader could not compile");
      }
      return shader;
    }
    function program(fragment: string) {
      const result = gl!.createProgram();
      if (!result) throw new Error("Program unavailable");
      programs.push(result);
      gl!.attachShader(result, compile(vertexShader, gl!.VERTEX_SHADER));
      gl!.attachShader(result, compile(fragment, gl!.FRAGMENT_SHADER));
      gl!.linkProgram(result);
      if (!gl!.getProgramParameter(result, gl!.LINK_STATUS)) {
        throw new Error("Aurora shader could not link");
      }
      return result;
    }
    function texture() {
      const result = gl!.createTexture();
      if (!result) throw new Error("Texture unavailable");
      textures.push(result);
      gl!.bindTexture(gl!.TEXTURE_2D, result);
      gl!.texParameteri(gl!.TEXTURE_2D, gl!.TEXTURE_MIN_FILTER, gl!.LINEAR);
      gl!.texParameteri(gl!.TEXTURE_2D, gl!.TEXTURE_MAG_FILTER, gl!.LINEAR);
      gl!.texParameteri(gl!.TEXTURE_2D, gl!.TEXTURE_WRAP_S, gl!.CLAMP_TO_EDGE);
      gl!.texParameteri(gl!.TEXTURE_2D, gl!.TEXTURE_WRAP_T, gl!.CLAMP_TO_EDGE);
      return result;
    }
    const atmosphere = program(atmosphereShader);
    const composite = program(compositeShader);
    const uniforms = (p: WebGLProgram, names: string[]) =>
      Object.fromEntries(names.map((name) => [name, gl.getUniformLocation(p, name)]));
    const fieldUniforms = uniforms(atmosphere, ["size", "anchor", "time"]);
    const compositeUniforms = uniforms(composite, ["size", "logoRect", "atmosphere", "logo"]);
    buffer = gl.createBuffer();
    if (!buffer) throw new Error("Buffer unavailable");
    gl.bindBuffer(gl.ARRAY_BUFFER, buffer);
    gl.bufferData(
      gl.ARRAY_BUFFER,
      new Float32Array([-1, -1, 1, -1, -1, 1, -1, 1, 1, -1, 1, 1]),
      gl.STATIC_DRAW,
    );
    const logoTexture = texture();
    gl.pixelStorei(gl.UNPACK_PREMULTIPLY_ALPHA_WEBGL, false);
    // Only alpha is used for the soft silhouette glow; the visible logo stays SVG.
    // Avoid mipmaps: implicit LOD derivatives are undefined inside the shader's
    // non-uniform bounds branch and can sample opaque coarse mip levels.
    const raster = document.createElement("canvas");
    raster.width = raster.height = 512;
    const context = raster.getContext("2d");
    if (!context) throw new Error("Logo rasterization unavailable");
    context.drawImage(image, 0, 0, 512, 512);
    gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, gl.RGBA, gl.UNSIGNED_BYTE, raster);
    const fieldTexture = texture();
    framebuffer = gl.createFramebuffer();
    if (!framebuffer) throw new Error("Framebuffer unavailable");
    let width = 1,
      height = 1,
      fieldWidth = 1,
      fieldHeight = 1;
    let quality = 0.85;
    let logoRect: [number, number, number, number] = [0, 0, 1, 1];
    const maxTexture = Math.min(gl.getParameter(gl.MAX_TEXTURE_SIZE) as number, 4096);

    const allocate = () => {
      const ratio =
        quality *
        Math.min(1, Math.sqrt(850000 / (width * height)), maxTexture / width, maxTexture / height);
      fieldWidth = Math.max(1, Math.round(width * ratio));
      fieldHeight = Math.max(1, Math.round(height * ratio));
      gl.bindTexture(gl.TEXTURE_2D, fieldTexture);
      gl.texImage2D(
        gl.TEXTURE_2D,
        0,
        gl.RGBA,
        fieldWidth,
        fieldHeight,
        0,
        gl.RGBA,
        gl.UNSIGNED_BYTE,
        null,
      );
      gl.bindFramebuffer(gl.FRAMEBUFFER, framebuffer);
      gl.framebufferTexture2D(gl.FRAMEBUFFER, gl.COLOR_ATTACHMENT0, gl.TEXTURE_2D, fieldTexture, 0);
      if (gl.checkFramebufferStatus(gl.FRAMEBUFFER) !== gl.FRAMEBUFFER_COMPLETE) {
        throw new Error("Aurora framebuffer unavailable");
      }
    };
    const fieldPosition = gl.getAttribLocation(atmosphere, "position");
    const compositePosition = gl.getAttribLocation(composite, "position");
    function use(p: WebGLProgram, position: number) {
      gl!.useProgram(p);
      gl!.bindBuffer(gl!.ARRAY_BUFFER, buffer);
      gl!.enableVertexAttribArray(position);
      gl!.vertexAttribPointer(position, 2, gl!.FLOAT, false, 0, 0);
    }
    return {
      resize(w, h, logo, scene) {
        width = w;
        height = h;
        const dpr = Math.min(window.devicePixelRatio || 1, 1.5, maxTexture / w, maxTexture / h);
        canvas.width = Math.max(1, Math.round(w * dpr));
        canvas.height = Math.max(1, Math.round(h * dpr));
        logoRect = [logo.left - scene.left, logo.top - scene.top, logo.width, logo.height];
        allocate();
      },
      draw(seconds) {
        gl.bindFramebuffer(gl.FRAMEBUFFER, framebuffer);
        gl.viewport(0, 0, fieldWidth, fieldHeight);
        use(atmosphere, fieldPosition);
        gl.uniform2f(fieldUniforms.size, width, height);
        gl.uniform2f(
          fieldUniforms.anchor,
          logoRect[0] + logoRect[2] / 2,
          logoRect[1] + logoRect[3] / 2,
        );
        gl.uniform1f(fieldUniforms.time, seconds);
        gl.drawArrays(gl.TRIANGLES, 0, 6);
        gl.bindFramebuffer(gl.FRAMEBUFFER, null);
        gl.viewport(0, 0, canvas.width, canvas.height);
        use(composite, compositePosition);
        gl.activeTexture(gl.TEXTURE0);
        gl.bindTexture(gl.TEXTURE_2D, fieldTexture);
        gl.uniform1i(compositeUniforms.atmosphere, 0);
        gl.activeTexture(gl.TEXTURE1);
        gl.bindTexture(gl.TEXTURE_2D, logoTexture);
        gl.uniform1i(compositeUniforms.logo, 1);
        gl.uniform2f(compositeUniforms.size, width, height);
        gl.uniform4f(compositeUniforms.logoRect, ...logoRect);
        gl.drawArrays(gl.TRIANGLES, 0, 6);
        gl.activeTexture(gl.TEXTURE0);
      },
      reduceQuality() {
        if (quality <= 0.45) return false;
        quality = Math.max(0.45, quality - 0.2);
        allocate();
        return true;
      },
      destroy,
    };
  } catch {
    destroy();
    return;
  }
}
