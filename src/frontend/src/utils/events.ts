// deno-lint-ignore-file no-window no-window-prefix

export const toast = {
  success: (message: string) =>
    window.dispatchEvent(
      new CustomEvent("toast", { detail: { content: message, type: "success" } }),
    ),
  error: (message: string) =>
    window.dispatchEvent(new CustomEvent("toast", { detail: { content: message, type: "error" } })),
  info: (message: string) =>
    window.dispatchEvent(new CustomEvent("toast", { detail: { content: message, type: "info" } })),
  warning: (message: string) =>
    window.dispatchEvent(
      new CustomEvent("toast", { detail: { content: message, type: "warning" } }),
    ),
};

export const overlay = {
  show: (message: string) =>
    window.dispatchEvent(
      new CustomEvent("overlay", { detail: { content: message, type: "show" } }),
    ),
  hide: () => window.dispatchEvent(new CustomEvent("overlay", { detail: { type: "hide" } })),
};
