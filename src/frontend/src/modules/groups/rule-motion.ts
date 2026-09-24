import { tick } from "svelte";

const running = new WeakMap<HTMLElement, Animation>();
const selector = ".group-list .rule[data-uuid]";

/** Match by rule ID so a move between groups can animate across recreated DOM nodes. */
export function captureRuleMove(movingRuleId: string) {
  if (typeof document === "undefined" || matchMedia("(prefers-reduced-motion: reduce)").matches) {
    return async () => {};
  }

  const nodes = [...document.querySelectorAll<HTMLElement>(selector)];
  const before = new Map(
    nodes
      .filter((node) => node.getClientRects().length)
      .map((node) => [node.dataset.uuid, node.getBoundingClientRect()]),
  );
  for (const node of nodes) running.get(node)?.cancel();

  return async () => {
    await tick();
    const targets = [...document.querySelectorAll<HTMLElement>(selector)]
      .filter((node) => node.getClientRects().length)
      .map((node) => ({ node, rect: node.getBoundingClientRect() }));

    for (const { node, rect } of targets) {
      const previous = before.get(node.dataset.uuid);
      if (!previous) continue;
      const x = previous.left - rect.left;
      const y = previous.top - rect.top;
      if (Math.abs(x) < 0.5 && Math.abs(y) < 0.5) continue;

      const zIndex = node.dataset.uuid === movingRuleId ? "2" : "1";
      const animation = node.animate(
        [
          { transform: `translate(${x}px, ${y}px)`, zIndex },
          { transform: "translate(0, 0)", zIndex },
        ],
        { duration: 200, easing: "cubic-bezier(0.22, 1, 0.36, 1)" },
      );
      running.set(node, animation);
    }
  };
}
