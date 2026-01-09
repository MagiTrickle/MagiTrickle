<script lang="ts">
  import { createEventDispatcher } from "svelte";
  import GenericDialog from "../../../components/ui/GenericDialog.svelte";
  import Button from "../../../components/ui/Button.svelte";
  import { toast } from "../../../utils/events";
  import { decodeGroupShare } from "../../../utils/group-share";
  import { t } from "../../../data/locale.svelte";
  import type { Group } from "../../../types";

  export let open = false;

  const dispatch = createEventDispatcher<{ import: { group: Group }; close: void }>();
  let importText = "";
  let triedSubmit = false;

  function handleKeydown(e: KeyboardEvent) {
    if ((e.metaKey || e.ctrlKey) && e.key === "Enter") {
      e.preventDefault();
      submit();
    }
  }

  function submit() {
    triedSubmit = true;
    if (!importText.trim()) return;

    try {
      const group = decodeGroupShare(importText);
      dispatch("import", { group });
      toast.success(t("Group imported"));
      close();
    } catch (error) {
      console.error("Error decoding group share", error);
      toast.error(t("Invalid group string"));
    }
  }

  function close() {
    importText = "";
    triedSubmit = false;
    dispatch("close");
  }
</script>

<svelte:window on:keydown={handleKeydown} />
<GenericDialog
  {open}
  {triedSubmit}
  title={t("Import Group")}
  textareaValue={importText}
  textareaPlaceholder={t("Paste group string")}
  maxWidth={520}
  on:close={close}
  on:textareaInput={(e) => (importText = e.detail)}
  on:submit={submit}
>
  <div slot="actions">
    <Button type="submit" on:click={submit}>{t("Import")}</Button>
  </div>
</GenericDialog>
