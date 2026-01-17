export function smoothReflow(node: HTMLElement, { duration = 200 } = {}) {
  let positions = new Map<HTMLElement, { left: number; top: number }>();
  let hasInitialized = false;
  let lastClientWidth = document.documentElement.clientWidth;

  const getPosition = (element: HTMLElement) => {
    return {
      left: element.offsetLeft,
      top: element.offsetTop,
    };
  };

  const savePositions = () => {
    const children = Array.from(node.children) as HTMLElement[];
    positions = new Map(children.map((child) => [child, getPosition(child)]));
  };

  const observer = new ResizeObserver(() => {
    const currentClientWidth = document.documentElement.clientWidth;
    const isResize = Math.abs(currentClientWidth - lastClientWidth) > 0.5;
    lastClientWidth = currentClientWidth;

    const children = Array.from(node.children) as HTMLElement[];
    const skipAnimation = !hasInitialized || isResize;

    children.forEach((child) => {
      if (child.hasAttribute("data-no-smooth-reflow")) return;

      const startPos = positions.get(child);
      const currentPos = getPosition(child);

      if (!startPos) return;

      const dx = startPos.left - currentPos.left;
      const dy = startPos.top - currentPos.top;

      if (Math.abs(dx) < 1 && Math.abs(dy) < 1) return;

      if (!skipAnimation) {
        child.animate(
          [{ transform: `translate(${dx}px, ${dy}px)` }, { transform: "translate(0, 0)" }],
          {
            duration,
            easing: "cubic-bezier(0.2, 0, 0.2, 1)",
            fill: "both",
          },
        );
      }
    });

    savePositions();
    hasInitialized = true;
  });

  savePositions();
  observer.observe(node);

  return {
    destroy() {
      observer.disconnect();
    },
  };
}
