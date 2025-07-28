<script lang="ts">
  import { createEventDispatcher } from "svelte";
  import GenericDialog from "../common/GenericDialog.svelte";
  import Button from "../common/Button.svelte";
  import { defaultRule } from "../../utils/defaults";
  import {
    isValidSubnet,
    isValidNamespace,
    isValidDomain,
    isValidWildcard,
    VALIDATOP_MAP,
  } from "../../utils/rule-validators";
  import type { Rule } from "../../types";
  import { toast } from "../../utils/events";

  export let open = false;
  export let group_index: number | null = null;

  const dispatch = createEventDispatcher();
  let import_rules_text = "";
  let triedSubmit = false;

  function handleKeydown(e: KeyboardEvent) {
    if ((e.metaKey || e.ctrlKey) && e.key === "Enter") {
      e.preventDefault();
      submit();
    }
  }

  function detectRuleType(pattern: string): keyof typeof VALIDATOP_MAP {
    const p = pattern.trim();

    if (isValidSubnet(p)) return "subnet";
    if (p.startsWith(".") && isValidNamespace(p.slice(1))) return "namespace";
    if (isValidDomain(p)) return "domain";
    if (isValidWildcard(p)) return "wildcard";

    return "regex";
  }

  function submit() {
    triedSubmit = true;

    if (!import_rules_text.trim() || group_index === null) return;

    const lines = import_rules_text
      .split("\n")
      .map((l) => l.trim())
      .filter(Boolean);
    if (!lines.length) return;

    const rules: Rule[] = lines.map((line) => ({
      ...defaultRule(),
      rule: line,
      type: detectRuleType(line),
    }));

    dispatch("import", { group_index, rules });
    toast.success(`Imported rules: ${rules.length}`);

    close();
  }

  function close() {
    import_rules_text = "";
    triedSubmit = false;
    dispatch("close");
  }
</script>

<svelte:window on:keydown={handleKeydown} />
<GenericDialog
  {open}
  {triedSubmit}
  title="Import Rule List"
  textareaValue={import_rules_text}
  textareaPlaceholder="Insert a list of IPs or domains, one per line"
  on:close={close}
  on:textareaInput={(e) => (import_rules_text = e.detail)}
  on:submit={submit}
>
  <div slot="actions">
    <Button type="submit" on:click={submit} style="font-size:.85rem;width:100%;">Import</Button>
  </div>
</GenericDialog>
