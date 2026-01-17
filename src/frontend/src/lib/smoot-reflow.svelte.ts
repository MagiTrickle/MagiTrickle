export function smoothReflow(node: HTMLElement, { duration = 200 } = {}) {
  let positions = new Map<HTMLElement, { left: number; top: number }>();

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
    const children = Array.from(node.children) as HTMLElement[];

    children.forEach((child) => {
      const startPos = positions.get(child);
      const currentPos = getPosition(child);

      if (!startPos) return;

      const dx = startPos.left - currentPos.left;
      const dy = startPos.top - currentPos.top;

      if (Math.abs(dx) < 1 && Math.abs(dy) < 1) return;

      child.animate(
        [{ transform: `translate(${dx}px, ${dy}px)` }, { transform: "translate(0, 0)" }],
        {
          duration,
          easing: "cubic-bezier(0.2, 0, 0.2, 1)",
          fill: "both",
        }
      );
    });

    savePositions();
  });

  savePositions();
  observer.observe(node);

  return {
    destroy() {
      observer.disconnect();
    },
  };
}
