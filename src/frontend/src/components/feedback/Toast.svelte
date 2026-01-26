<script lang="ts">
  import { onDestroy, onMount } from "svelte";
  import { flip } from "svelte/animate";
  import { cubicInOut } from "svelte/easing";

  import { CircleCheck, CircleInfo, CircleX } from "../ui/icons";

  type ToastType = "info" | "success" | "error" | "warning";
  type ToastItem = {
    id: string;
    content: string;
    type: ToastType;
  };

  const MAX_TOASTS = 3;

  let toasts = $state<ToastItem[]>([]);
  const timers = new Map<string, number>();

  function toastAccent(type: ToastType) {
    switch (type) {
      case "info":
        return "var(--accent)";
      case "success":
        return "var(--green-vibrant)";
      case "error":
        return "var(--red)";
      case "warning":
        return "var(--yellow-bright)";
    }
  }

  function backOut(t: number) {
    const c1 = 1.70158;
    const c3 = c1 + 1;
    return 1 + c3 * Math.pow(t - 1, 3) + c1 * Math.pow(t - 1, 2);
  }

  function toastEnter(node: Element) {
    const opacity = Number(getComputedStyle(node).opacity);
    return {
      duration: 320,
      easing: backOut,
      css: (t: number) => {
        const blurValue = Math.max(0, (1 - t) * 4);
        const scaleValue = 0.96 + t * (1 - 0.96);
        const opacityValue = Math.max(0, Math.min(1, t * opacity));

        return `
        transform: translate3d(0, ${(1 - t) * -26}px, 0) scale(${scaleValue});
        opacity: ${opacityValue};
        filter: blur(${blurValue}px);
      `;
      },
    };
  }

  function toastLeave(node: Element) {
    const opacity = Number(getComputedStyle(node).opacity);
    const isLast = (node as HTMLElement).dataset.last === "true";
    return {
      duration: isLast ? 420 : 220,
      delay: isLast ? 70 : 0,
      css: (t: number, u: number) => {
        const eased = cubicInOut(u);
        return `transform: translate3d(0, ${
          eased * 20 * (isLast ? 1.8 : 1) * (isLast ? 1 : -1)
        }px, 0) scale(${1 - (1 - 0.985) * eased}); opacity: ${
          (1 - eased) * opacity
        }; filter: blur(${eased}px);`;
      },
    };
  }

  function clearToastTimer(id: string) {
    const timer = timers.get(id);
    if (timer) {
      window.clearTimeout(timer);
      timers.delete(id);
    }
  }

  function removeToast(id: string) {
    clearToastTimer(id);
    toasts = toasts.filter((toast) => toast.id !== id);
  }

  function createId() {
    if (typeof crypto !== "undefined" && "randomUUID" in crypto) {
      return crypto.randomUUID();
    }
    return `${Date.now().toString(16)}${Math.random().toString(16).slice(2, 8)}`;
  }

  function scheduleToastRemoval(id: string) {
    clearToastTimer(id);
    const timer = window.setTimeout(() => removeToast(id), 3000);
    timers.set(id, timer);
  }

  function onToastEvent(event: WindowEventMap["toast"]) {
    const nextToast: ToastItem = {
      id: createId(),
      content: event.detail.content,
      type: event.detail.type,
    };

    const nextToasts = [nextToast, ...toasts];
    nextToasts.slice(MAX_TOASTS).forEach((toast) => clearToastTimer(toast.id));
    toasts = nextToasts.slice(0, MAX_TOASTS);
    scheduleToastRemoval(nextToast.id);
  }

  onMount(() => {
    window.addEventListener("toast", onToastEvent);
  });

  onDestroy(() => {
    window.removeEventListener("toast", onToastEvent);
    timers.forEach((timer) => window.clearTimeout(timer));
    timers.clear();
  });
</script>

<div class="stack" aria-live="polite">
  {#each toasts as toast, index (toast.id)}
    <div class="toast-slot" animate:flip={{ duration: 240, easing: cubicInOut }}>
      <div
        class="toast"
        data-last={index === toasts.length - 1}
        style={`--toast-accent: ${toastAccent(toast.type)};`}
        in:toastEnter
        out:toastLeave
      >
        <div class="icon">
          {#if toast.type === "success"}
            <CircleCheck size={20} />
          {:else if toast.type === "error"}
            <CircleX size={20} />
          {:else}
            <CircleInfo size={20} />
          {/if}
        </div>
        <div class="content">
          {toast.content}
        </div>
      </div>
    </div>
  {/each}
</div>

<style>
  .stack {
    position: fixed;
    top: 50px;
    left: 50%;
    transform: translateX(-50%);
    display: flex;
    flex-direction: column;
    gap: 0.6rem;
    z-index: 9999;
    align-items: center;
  }

  .toast-slot {
    display: flex;
    justify-content: center;
    width: 100%;
  }

  .toast {
    display: flex;
    justify-content: center;
    align-items: center;
    min-width: 200px;
    max-width: 400px;
    padding: 15px;
    border-radius: 10px;
    border: 1px solid var(--bg-light-extra);
    border-left: 4px solid var(--toast-accent);
    box-shadow: 0 12px 28px rgba(0, 0, 0, 0.45);
    background: var(--bg-light);
    backdrop-filter: blur(6px);
    will-change: transform, opacity, filter;
  }

  .content {
    color: var(--text);
    font-weight: 600;
    display: flex;
    justify-content: center;
    align-items: center;
  }

  .icon {
    color: var(--toast-accent);
    position: relative;
    top: 2px;
    margin-right: 0.3rem;
  }
</style>
