export function smoothReflow(node: HTMLElement, { duration = 200 } = {}) {
  let positions = new Map<HTMLElement, { left: number; top: number }>()
  const runningAnimations = new WeakMap<HTMLElement, Animation>()
  let hasInitialized = false
  let lastClientWidth = document.documentElement.clientWidth

  const shouldAnimate = (element: HTMLElement) => {
    if (element.hasAttribute("data-reflow-skip")) return false
    const position = getComputedStyle(element).position
    return position !== "sticky" && position !== "fixed"
  }

  const getPosition = (element: HTMLElement, parentRect: DOMRect) => {
    const rect = element.getBoundingClientRect()
    return {
      left: rect.left - parentRect.left,
      top: rect.top - parentRect.top,
    }
  }

  const savePositions = () => {
    const children = Array.from(node.children) as HTMLElement[]
    const parentRect = node.getBoundingClientRect()
    positions = new Map(
      children
        .filter((child) => child.getClientRects().length > 0 && shouldAnimate(child))
        .map((child) => [child, getPosition(child, parentRect)]),
    )
  }

  const observer = new ResizeObserver(() => {
    const currentClientWidth = document.documentElement.clientWidth
    const isResize = Math.abs(currentClientWidth - lastClientWidth) > 0.5
    lastClientWidth = currentClientWidth

    const children = Array.from(node.children) as HTMLElement[]
    const parentRect = node.getBoundingClientRect()
    const skipAnimation = !hasInitialized || isResize
    const currentPositions = new Map<HTMLElement, { left: number; top: number }>()

    children.forEach((child) => {
      if (!shouldAnimate(child)) return
      if (child.getClientRects().length === 0) return

      const startPos = positions.get(child)
      const currentPos = getPosition(child, parentRect)
      currentPositions.set(child, currentPos)

      if (!startPos) return

      const dx = startPos.left - currentPos.left
      const dy = startPos.top - currentPos.top

      if (Math.abs(dx) < 0.5 && Math.abs(dy) < 0.5) return

      if (!skipAnimation) {
        runningAnimations.get(child)?.cancel()
        const animation = child.animate(
          [{ transform: `translate(${dx}px, ${dy}px)` }, { transform: "translate(0, 0)" }],
          {
            duration,
            easing: "cubic-bezier(0.2, 0, 0.2, 1)",
            fill: "both",
          },
        )
        runningAnimations.set(child, animation)
      }
    })

    positions = currentPositions
    hasInitialized = true
  })

  savePositions()
  observer.observe(node)

  return {
    destroy() {
      observer.disconnect()
    },
  }
}
