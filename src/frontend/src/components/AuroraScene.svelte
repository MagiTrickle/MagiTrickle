<script lang="ts">
  import logoUrl from "../../../../img/logo.svg";
  import { createAuroraRenderer, type AuroraRenderer } from "../lib/aurora/renderer";

  type Props = {
    root: HTMLDivElement | undefined;
    anchor: HTMLDivElement | undefined;
    ready?: boolean;
  };
  let { root, anchor, ready = $bindable(false) }: Props = $props();
  let canvas: HTMLCanvasElement;

  $effect(() => {
    if (!root || !anchor || !canvas) return;
    const scene = root;
    const mark = anchor;
    const surface = canvas;
    const motion = matchMedia("(prefers-reduced-motion: reduce)");
    const image = new Image();
    let renderer: AuroraRenderer | undefined;
    let disposed = false;
    let frame = 0;
    let lastTime = 0;
    let elapsed = 12;
    let samples = 0;
    let sampleTime = 0;
    let staticQuality = false;
    let slowFrames = false;
    let lastDraw = 0;

    const stop = () => {
      cancelAnimationFrame(frame);
      frame = 0;
      lastTime = 0;
      samples = 0;
      sampleTime = 0;
    };
    const fallback = () => {
      stop();
      renderer?.destroy();
      renderer = undefined;
      ready = false;
      surface.dataset.mode = "fallback";
    };
    const draw = () => {
      try {
        renderer?.draw(elapsed);
      } catch {
        fallback();
      }
    };
    const tick = (now: number) => {
      if (disposed || !renderer) return;
      const delta = lastTime ? now - lastTime : 0;
      lastTime = now;
      elapsed += Math.min(delta, 64) / 1000;
      if (!slowFrames || now - lastDraw >= 32) {
        draw();
        lastDraw = now;
      }
      // Sustained frame pressure lowers only the expensive atmosphere pass.
      // Very slow devices settle on a still scene rather than burning resources.
      if (delta > 0) {
        sampleTime += delta;
        samples++;
      }
      if (samples >= 120) {
        const average = sampleTime / samples;
        try {
          if (average > 26 && !renderer?.reduceQuality()) {
            if (average > 55) staticQuality = true;
            else slowFrames = true;
          }
        } catch {
          fallback();
        }
        samples = 0;
        sampleTime = 0;
      }
      if (renderer && !staticQuality) frame = requestAnimationFrame(tick);
      else surface.dataset.mode = renderer ? "static" : "fallback";
    };
    const updateMotion = () => {
      if (disposed) return;
      stop();
      if (!renderer) return;
      if (motion.matches || staticQuality) {
        surface.dataset.mode = "static";
        draw();
      } else if (document.hidden || scene.contains(document.activeElement)) {
        surface.dataset.mode = "paused";
      } else {
        surface.dataset.mode = "animated";
        frame = requestAnimationFrame(tick);
      }
    };
    const measure = () => {
      if (disposed) return;
      const bounds = scene.getBoundingClientRect();
      const logoBounds = mark.getBoundingClientRect();
      scene.style.setProperty(
        "--aurora-anchor-y",
        `${logoBounds.top - bounds.top + logoBounds.height / 2}px`,
      );
      if (!renderer || !bounds.width || !bounds.height) return;
      try {
        renderer.resize(bounds.width, bounds.height, logoBounds, bounds);
        draw();
      } catch {
        fallback();
      }
    };
    const initialize = () => {
      if (disposed) return;
      renderer?.destroy();
      renderer = createAuroraRenderer(surface, image);
      if (!renderer) {
        fallback();
        return;
      }
      measure();
      ready = !!renderer;
      updateMotion();
    };
    const focusChanged = () => queueMicrotask(updateMotion);
    const contextLost = (event: Event) => {
      event.preventDefault();
      fallback();
    };
    surface.addEventListener("webglcontextlost", contextLost);
    surface.addEventListener("webglcontextrestored", initialize);
    document.addEventListener("visibilitychange", updateMotion);
    motion.addEventListener("change", updateMotion);
    scene.addEventListener("focusin", focusChanged);
    scene.addEventListener("focusout", focusChanged);
    window.addEventListener("resize", measure);
    const observer = new ResizeObserver(measure);
    observer.observe(scene);
    observer.observe(mark);
    if (mark.parentElement) observer.observe(mark.parentElement);
    image.onload = initialize;
    image.onerror = fallback;
    image.src = logoUrl;
    measure();
    void document.fonts.ready.then(measure);

    return () => {
      disposed = true;
      stop();
      observer.disconnect();
      renderer?.destroy();
      image.onload = null;
      image.onerror = null;
      surface.removeEventListener("webglcontextlost", contextLost);
      surface.removeEventListener("webglcontextrestored", initialize);
      document.removeEventListener("visibilitychange", updateMotion);
      motion.removeEventListener("change", updateMotion);
      scene.removeEventListener("focusin", focusChanged);
      scene.removeEventListener("focusout", focusChanged);
      window.removeEventListener("resize", measure);
    };
  });
</script>

<div class="still" aria-hidden="true"></div>
<canvas bind:this={canvas} class:ready aria-hidden="true" data-mode="fallback"></canvas>

<style>
  /* Lightweight fallback for browsers without WebGL. The full moving light
     field remains shader-driven, so no baked full-screen bitmap is shipped. */
  .still {
    position: absolute;
    width: max(100%, 1000px);
    aspect-ratio: 8 / 5;
    left: 50%;
    top: var(--aurora-anchor-y, 30vh);
    transform: translate(-50%, -31.58%);
    pointer-events: none;
    background:
      radial-gradient(ellipse 54% 25% at 31% 44%, rgba(22, 105, 157, 0.16), transparent 82%),
      radial-gradient(ellipse 38% 32% at 73% 29%, rgba(55, 58, 151, 0.11), transparent 82%);
    -webkit-mask-image: linear-gradient(transparent, #000 10%, #000 85%, transparent);
    mask-image: linear-gradient(transparent, #000 10%, #000 85%, transparent);
  }

  .still::before {
    content: "";
    position: absolute;
    inset: 4% 0 10%;
    background:
      radial-gradient(ellipse 60% 17% at 46% 40%, rgba(26, 134, 188, 0.2), transparent 88%),
      linear-gradient(
        162deg,
        transparent 29%,
        rgba(18, 71, 146, 0.11) 36%,
        rgba(30, 135, 212, 0.22) 45%,
        transparent 58%
      ),
      linear-gradient(
        173deg,
        transparent 26%,
        rgba(21, 93, 156, 0.12) 39%,
        rgba(51, 164, 192, 0.17) 45%,
        transparent 60%
      );
    filter: blur(24px);
    transform: rotate(-6deg);
  }

  .still::after {
    content: "";
    position: absolute;
    inset: 7%;
    background:
      radial-gradient(circle at 8% 21%, #aac9ec 0 0.8px, transparent 1.7px),
      radial-gradient(circle at 15% 69%, #a0c6ed 0 0.6px, transparent 1.4px),
      radial-gradient(circle at 26% 17%, #b2d5f1 0 0.6px, transparent 1.5px),
      radial-gradient(circle at 35% 55%, #95c9ec 0 0.7px, transparent 1.6px),
      radial-gradient(circle at 43% 33%, #a9cdf8 0 0.6px, transparent 1.4px),
      radial-gradient(circle at 57% 14%, #d5ecfb 0 0.8px, transparent 2px),
      radial-gradient(circle at 65% 67%, #82b1e0 0 0.6px, transparent 1.5px),
      radial-gradient(circle at 74% 21%, #9eb9dc 0 0.7px, transparent 1.6px),
      radial-gradient(circle at 86% 46%, #bdd9f2 0 0.7px, transparent 1.6px),
      radial-gradient(circle at 93% 79%, #91c4eb 0 0.8px, transparent 1.7px);
    opacity: 0.52;
  }

  canvas {
    position: absolute;
    inset: 0;
    width: 100%;
    height: 100%;
    pointer-events: none;
    opacity: 0;
  }
  canvas.ready {
    opacity: 1;
  }
</style>
