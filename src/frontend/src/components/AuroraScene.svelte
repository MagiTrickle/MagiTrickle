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
  /* Same light field at t=12, 1600×1000, with the logo composed separately.
     Anchor the baked focal point to the live logo on short and narrow screens. */
  .still {
    position: absolute;
    width: max(100%, 1000px);
    aspect-ratio: 8 / 5;
    left: 50%;
    top: var(--aurora-anchor-y, 30vh);
    transform: translate(-50%, -30.98%);
    background: url("../assets/aurora-still.webp") center / 100% 100% no-repeat;
    -webkit-mask-image: linear-gradient(transparent, #000 10%, #000 85%, transparent);
    mask-image: linear-gradient(transparent, #000 10%, #000 85%, transparent);
    pointer-events: none;
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
