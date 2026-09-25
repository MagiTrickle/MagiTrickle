import { t } from "../data/locale.svelte";

import { toast } from "./events";

export function copyRulePatternsToClipboard(rules: readonly { rule: string }[]) {
  const patterns = rules.map((rule) => rule.rule.trim()).filter(Boolean);

  if (patterns.length === 0) {
    toast.error(t("Nothing to copy"));
    return;
  }

  const textarea = document.createElement("textarea");

  try {
    textarea.value = patterns.join("\n");
    textarea.style.position = "fixed";
    textarea.style.left = "-9999px";

    document.body.appendChild(textarea);
    textarea.select();

    if (!document.execCommand("copy")) {
      throw new Error("Copy command failed");
    }

    toast.success(t("Copied to clipboard"));
  } catch (e) {
    console.error("Failed to copy to clipboard:", e);
    toast.error(t("Failed to copy"));
  } finally {
    textarea.remove();
  }
}
