<script lang="ts">
  import { onMount } from "svelte";

  type Props = {
    variant?: "front" | "back";
  };

  let { variant = "front" }: Props = $props();

  type Flake = {
    x: number;
    y: number;
    r: number;
    vy: number;
    vx: number;
    alpha: number;
    phase: number;
    swing: number;
  };

  const layerConfig =
    variant === "back"
      ? {
          densityScale: 0.24,
          minCount: 8,
          maxCount: 60,
          sizeScale: 0.45,
          speedScale: 0.45,
          alphaScale: 0.4,
          swingScale: 0.45,
          windScale: 0.35,
          opacity: 0.42,
          zIndex: 0,
          fps: { normal: 32, low: 24 },
        }
      : {
          densityScale: 0.7,
          minCount: 20,
          maxCount: 120,
          sizeScale: 1,
          speedScale: 1,
          alphaScale: 1,
          swingScale: 1,
          windScale: 1,
          opacity: 0.85,
          zIndex: 10,
          fps: { normal: 60, low: 40 },
        };

  let canvas: HTMLCanvasElement | null = null;
  let gl: WebGLRenderingContext | null = null;
  let program: WebGLProgram | null = null;
  let buffer: WebGLBuffer | null = null;
  let attribPos = -1;
  let attribSize = -1;
  let attribAlpha = -1;
  let uniformResolution: WebGLUniformLocation | null = null;
  let uniformPixelRatio: WebGLUniformLocation | null = null;
  let flakes: Flake[] = [];
  let vertexData = new Float32Array(0);
  let qualityLevel = 0;
  let qualityTimer = 0;
  let frameSamples: number[] = [];
  let lastSampleTime = 0;
  let raf = 0;
  let lastTime = 0;
  let lastFrame = 0;
  let width = 0;
  let height = 0;
  let wind = 0;
  let windTarget = 0;
  let windTimer = 0;
  let reducedMotion = false;
  let lowPower = false;
  let motionFactor = 1;
  let dpr = 1;

  const baseDensity = 0.000075;
  const tau = Math.PI * 2;

  function createShader(type: number, source: string) {
    if (!gl) return null;
    const shader = gl.createShader(type);
    if (!shader) return null;
    gl.shaderSource(shader, source);
    gl.compileShader(shader);
    if (!gl.getShaderParameter(shader, gl.COMPILE_STATUS)) {
      gl.deleteShader(shader);
      return null;
    }
    return shader;
  }

  function createProgram(vertexSource: string, fragmentSource: string) {
    if (!gl) return null;
    const vertexShader = createShader(gl.VERTEX_SHADER, vertexSource);
    const fragmentShader = createShader(gl.FRAGMENT_SHADER, fragmentSource);
    if (!vertexShader || !fragmentShader) return null;
    const nextProgram = gl.createProgram();
    if (!nextProgram) return null;
    gl.attachShader(nextProgram, vertexShader);
    gl.attachShader(nextProgram, fragmentShader);
    gl.linkProgram(nextProgram);
    gl.deleteShader(vertexShader);
    gl.deleteShader(fragmentShader);
    if (!gl.getProgramParameter(nextProgram, gl.LINK_STATUS)) {
      gl.deleteProgram(nextProgram);
      return null;
    }
    return nextProgram;
  }

  function initGL() {
    if (!canvas) return;
    gl =
      canvas.getContext("webgl", {
        alpha: true,
        antialias: false,
        premultipliedAlpha: true,
        depth: false,
        stencil: false,
      }) ?? null;
    if (!gl) return;

    const vertexSource = `
      attribute vec2 a_pos;
      attribute float a_size;
      attribute float a_alpha;
      uniform vec2 u_resolution;
      uniform float u_pixelRatio;
      varying float v_alpha;
      void main() {
        vec2 zeroToOne = a_pos / u_resolution;
        vec2 clip = zeroToOne * 2.0 - 1.0;
        gl_Position = vec4(clip * vec2(1.0, -1.0), 0.0, 1.0);
        gl_PointSize = a_size * u_pixelRatio;
        v_alpha = a_alpha;
      }
    `;

    const fragmentSource = `
      precision mediump float;
      varying float v_alpha;
      void main() {
        vec2 coord = gl_PointCoord - vec2(0.5);
        float dist = length(coord);
        float alpha = smoothstep(0.5, 0.0, dist);
        gl_FragColor = vec4(1.0, 1.0, 1.0, alpha * v_alpha);
      }
    `;

    program = createProgram(vertexSource, fragmentSource);
    if (!program) {
      gl = null;
      return;
    }

    buffer = gl.createBuffer();
    attribPos = gl.getAttribLocation(program, "a_pos");
    attribSize = gl.getAttribLocation(program, "a_size");
    attribAlpha = gl.getAttribLocation(program, "a_alpha");
    uniformResolution = gl.getUniformLocation(program, "u_resolution");
    uniformPixelRatio = gl.getUniformLocation(program, "u_pixelRatio");

    gl.useProgram(program);
    gl.enable(gl.BLEND);
    gl.blendFunc(gl.SRC_ALPHA, gl.ONE_MINUS_SRC_ALPHA);
    gl.clearColor(0, 0, 0, 0);
  }

  function clamp(value: number, min: number, max: number) {
    return Math.min(max, Math.max(min, value));
  }

  function computeDeviceHints() {
    const mem = (navigator as Navigator & { deviceMemory?: number }).deviceMemory ?? 4;
    const cores = navigator.hardwareConcurrency ?? 4;
    lowPower = mem <= 4 || cores <= 4;
    qualityLevel = lowPower ? Math.max(qualityLevel, 1) : qualityLevel;
    const maxDpr = lowPower ? 1.3 : 1.8;
    dpr = Math.min(maxDpr, window.devicePixelRatio || 1);
  }

  function computeCount(w: number, h: number) {
    const area = w * h;
    const mem = (navigator as Navigator & { deviceMemory?: number }).deviceMemory ?? 4;
    const memFactor = mem <= 2 ? 0.5 : mem <= 4 ? 0.75 : 1;
    const motionCountFactor = reducedMotion ? 0.45 : 1;
    const count = Math.round(
      area * baseDensity * layerConfig.densityScale * memFactor * motionCountFactor,
    );
    const min = reducedMotion
      ? Math.max(6, Math.round(layerConfig.minCount * 0.6))
      : layerConfig.minCount;
    return clamp(count, min, layerConfig.maxCount);
  }

  function resetFlake(flake: Flake, randomizeY = true) {
    const depth = Math.pow(Math.random(), 0.7);
    flake.r = (0.6 + depth * 2.8) * layerConfig.sizeScale;
    flake.vy = (18 + depth * 90) * layerConfig.speedScale;
    flake.vx = (Math.random() * 2 - 1) * (6 + depth * 12) * layerConfig.speedScale;
    flake.alpha = Math.min(1, (0.3 + depth * 0.6) * layerConfig.alphaScale);
    flake.swing = (6 + depth * 18) * layerConfig.swingScale;
    flake.phase = Math.random() * tau;
    flake.x = Math.random() * width;
    flake.y = randomizeY ? Math.random() * height : -flake.r - Math.random() * height * 0.2;
  }

  function createFlake(randomizeY = true) {
    const flake: Flake = {
      x: 0,
      y: 0,
      r: 0,
      vy: 0,
      vx: 0,
      alpha: 0,
      phase: 0,
      swing: 0,
    };
    resetFlake(flake, randomizeY);
    return flake;
  }

  function resize() {
    if (!canvas || !gl) return;
    width = window.innerWidth;
    height = window.innerHeight;
    if (width <= 0 || height <= 0) return;

    canvas.width = Math.floor(width * dpr);
    canvas.height = Math.floor(height * dpr);
    canvas.style.width = `${width}px`;
    canvas.style.height = `${height}px`;
    gl.viewport(0, 0, canvas.width, canvas.height);
    gl.useProgram(program);
    if (uniformResolution) gl.uniform2f(uniformResolution, width, height);
    if (uniformPixelRatio) gl.uniform1f(uniformPixelRatio, dpr);

    const nextCount = computeCount(width, height);
    if (flakes.length > nextCount) {
      flakes.length = nextCount;
    } else {
      while (flakes.length < nextCount) {
        flakes.push(createFlake());
      }
    }

  }

  function updateWind(dt: number) {
    windTimer += dt;
    if (windTimer > 3 + Math.random() * 4) {
      windTimer = 0;
      windTarget = (Math.random() * 2 - 1) * (lowPower ? 18 : 28) * layerConfig.windScale;
    }
    wind += (windTarget - wind) * (lowPower ? 0.04 : 0.02);
  }

  function frame(time: number) {
    if (!gl || !canvas || !program || !buffer) return;
    if (!lastTime) lastTime = time;

    const maxFps = lowPower ? layerConfig.fps.low : layerConfig.fps.normal;
    if (time - lastFrame < 1000 / maxFps) {
      raf = requestAnimationFrame(frame);
      return;
    }
    lastFrame = time;

    const dt = Math.min(0.032, (time - lastTime) / 1000);
    lastTime = time;

    updateWind(dt);

    gl.clear(gl.COLOR_BUFFER_BIT);

    const step = qualityLevel === 2 ? 3 : qualityLevel === 1 ? 2 : 1;
    const vertexStride = 4;
    const maxCount = Math.ceil(flakes.length / step);
    const needed = maxCount * vertexStride;
    if (vertexData.length < needed) {
      vertexData = new Float32Array(needed);
    }
    let vertexIndex = 0;

    for (let i = 0; i < flakes.length; i += step) {
      const flake = flakes[i];
      const sway = Math.sin(time * 0.001 + flake.phase) * flake.swing;
      flake.x += (flake.vx + wind + sway) * dt * motionFactor;
      flake.y += flake.vy * dt * motionFactor;

      if (flake.y > height + flake.r) {
        resetFlake(flake, false);
      }

      if (flake.x < -flake.r) {
        flake.x = width + flake.r;
      } else if (flake.x > width + flake.r) {
        flake.x = -flake.r;
      }

      vertexData[vertexIndex++] = flake.x;
      vertexData[vertexIndex++] = flake.y;
      vertexData[vertexIndex++] = flake.r * 2.2;
      vertexData[vertexIndex++] = flake.alpha;
    }
    const drawCount = vertexIndex / vertexStride;
    if (drawCount > 0) {
      gl.useProgram(program);
      gl.bindBuffer(gl.ARRAY_BUFFER, buffer);
      gl.bufferData(gl.ARRAY_BUFFER, vertexData.subarray(0, vertexIndex), gl.DYNAMIC_DRAW);
      gl.enableVertexAttribArray(attribPos);
      gl.enableVertexAttribArray(attribSize);
      gl.enableVertexAttribArray(attribAlpha);
      const stride = vertexStride * 4;
      gl.vertexAttribPointer(attribPos, 2, gl.FLOAT, false, stride, 0);
      gl.vertexAttribPointer(attribSize, 1, gl.FLOAT, false, stride, 8);
      gl.vertexAttribPointer(attribAlpha, 1, gl.FLOAT, false, stride, 12);
      gl.drawArrays(gl.POINTS, 0, drawCount);
    }
    if (!lastSampleTime) lastSampleTime = time;
    const frameTime = time - lastSampleTime;
    if (frameTime > 0 && frameTime < 200) {
      frameSamples.push(frameTime);
      if (frameSamples.length > 24) frameSamples.shift();
    }
    lastSampleTime = time;
    qualityTimer += dt;
    if (qualityTimer > 1 && frameSamples.length >= 12) {
      qualityTimer = 0;
      const avg = frameSamples.reduce((sum, value) => sum + value, 0) / frameSamples.length;
      if (avg > 32) {
        qualityLevel = 2;
      } else if (avg > 22 || lowPower) {
        qualityLevel = Math.max(qualityLevel, 1);
      } else {
        qualityLevel = 0;
      }
    }
    raf = requestAnimationFrame(frame);
  }

  function start() {
    if (raf) return;
    lastTime = 0;
    lastFrame = 0;
    raf = requestAnimationFrame(frame);
  }

  function stop() {
    if (!raf) return;
    cancelAnimationFrame(raf);
    raf = 0;
  }

  onMount(() => {
    if (!canvas) return;

    computeDeviceHints();
    initGL();
    if (!gl) return;

    const motionMedia = window.matchMedia("(prefers-reduced-motion: reduce)");
    const updateMotion = () => {
      reducedMotion = motionMedia.matches;
      motionFactor = reducedMotion ? 0.45 : 1;
      resize();
    };
    updateMotion();
    if ("addEventListener" in motionMedia) {
      motionMedia.addEventListener("change", updateMotion);
    } else {
      motionMedia.addListener(updateMotion);
    }

    const handleResize = () => {
      computeDeviceHints();
      resize();
    };
    window.addEventListener("resize", handleResize);

    const handleVisibility = () => {
      if (document.hidden) {
        stop();
      } else {
        start();
      }
    };
    document.addEventListener("visibilitychange", handleVisibility);

    resize();
    start();

    return () => {
      stop();
      window.removeEventListener("resize", handleResize);
      document.removeEventListener("visibilitychange", handleVisibility);
      if ("removeEventListener" in motionMedia) {
        motionMedia.removeEventListener("change", updateMotion);
      } else {
        motionMedia.removeListener(updateMotion);
      }
      if (gl) {
        if (buffer) gl.deleteBuffer(buffer);
        if (program) gl.deleteProgram(program);
      }
      buffer = null;
      program = null;
      gl = null;
    };
  });
</script>

<canvas
  bind:this={canvas}
  class="snow-field"
  style={`--snow-z:${layerConfig.zIndex}; --snow-opacity:${layerConfig.opacity};`}
  aria-hidden="true"
></canvas>

<style>
  .snow-field {
    position: fixed;
    inset: 0;
    width: 100%;
    height: 100%;
    pointer-events: none;
    z-index: var(--snow-z, 10);
    opacity: var(--snow-opacity, 0.85);
  }

  @media (prefers-reduced-motion: reduce) {
    .snow-field {
      opacity: calc(var(--snow-opacity, 0.85) * 0.7);
    }
  }
</style>
