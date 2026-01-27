import { tick } from "svelte";

export const tooltip = $state({
  visible: false,
  text: "",
  x: 0,
  y: 0,
  opacity: 0,
});

export async function show(anchor: HTMLElement, text: string) {
  tooltip.text = text;
  tooltip.visible = true;
  tooltip.opacity = 0;

  await tick();

  const tRect = anchor.getBoundingClientRect();
  const tooltipEl = document.getElementById("global-tooltip");
  if (!tooltipEl) return;

  const mRect = tooltipEl.getBoundingClientRect();
  const PAD = 8;

  let top = tRect.top - mRect.height - 6;
  let left = tRect.left + (tRect.width - mRect.width) / 2;

  if (top < PAD) top = tRect.bottom + 6;
  left = Math.max(PAD, Math.min(left, globalThis.innerWidth - mRect.width - PAD));

  tooltip.x = left;
  tooltip.y = top;
  tooltip.opacity = 1;
}

export function hide() {
  tooltip.visible = false;
}

globalThis.addEventListener("scroll", hide);
