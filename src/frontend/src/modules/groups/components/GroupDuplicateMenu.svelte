<script lang="ts">
  import Button from "../../../components/ui/Button.svelte";
  import DropdownMenu from "../../../components/ui/DropdownMenu.svelte";
  import Tooltip from "../../../components/ui/Tooltip.svelte";
  import { t } from "../../../data/locale.svelte";
  import { TriangleAlert } from "../../../components/ui/icons";
  import type { GroupDuplicateConflict } from "../groups.svelte";

  type Props = {
    conflicts: GroupDuplicateConflict[];
    isRuleHighlighted: (ruleId: string) => boolean;
    onConflictClick: (groupId: string, ruleId: string) => void;
  };

  let { conflicts, isRuleHighlighted, onConflictClick }: Props = $props();

  function duplicateConflictGroupTitle(conflict: GroupDuplicateConflict) {
    const normalizedName = conflict.groupName.trim();
    if (normalizedName) return normalizedName;
    return t("Unnamed group");
  }

  function duplicateConflictGroupColor(conflict: GroupDuplicateConflict) {
    return conflict.groupColor || "var(--bg-light-extra)";
  }
</script>

{#if conflicts.length > 0}
  <DropdownMenu>
    {#snippet trigger()}
      <Tooltip value={t("Contains duplicate rules")}>
        <span
          aria-label={t("Show duplicate conflicts")}
          class="group-duplicate-indicator group-duplicate-menu-trigger"
        >
          <TriangleAlert size={18} />
        </span>
      </Tooltip>
    {/snippet}
    {#snippet item1()}
      <div class="group-duplicate-menu-list">
        {#each conflicts as conflict (conflict.ruleId)}
          <Button
            general
            class={isRuleHighlighted(conflict.ruleId)
              ? "duplicate-conflict-button active"
              : "duplicate-conflict-button"}
            onclick={() => onConflictClick(conflict.groupId, conflict.ruleId)}
          >
            <div class="dd-label duplicate-conflict-label">
              {#if conflict.ruleName.trim()}
                <div class="duplicate-conflict-title">
                  <span class="duplicate-conflict-name">{conflict.ruleName.trim()}</span>
                </div>
                <div class="duplicate-conflict-subtitle">
                  <span class="duplicate-conflict-group">
                    <span
                      class="group-color-dot"
                      style="background-color: {duplicateConflictGroupColor(conflict)}"
                    ></span>
                    {duplicateConflictGroupTitle(conflict)}
                  </span>
                  <span class="duplicate-conflict-type">{conflict.ruleType}</span>
                  <span class="duplicate-conflict-pattern">{conflict.rulePattern}</span>
                </div>
              {:else}
                <div class="duplicate-conflict-title">
                  <span class="duplicate-conflict-type">{conflict.ruleType}</span>
                  <span class="duplicate-conflict-pattern-title"
                    >{conflict.rulePattern || t("Unnamed rule")}</span
                  >
                </div>
                <div class="duplicate-conflict-subtitle">
                  <span class="duplicate-conflict-group">
                    <span
                      class="group-color-dot"
                      style="background-color: {duplicateConflictGroupColor(conflict)}"
                    ></span>
                    {duplicateConflictGroupTitle(conflict)}
                  </span>
                </div>
              {/if}
            </div>
            <div class="duplicate-conflict-count">
              x{conflict.totalRulesWithSameKey}
            </div>
          </Button>
        {/each}
      </div>
    {/snippet}
  </DropdownMenu>
{/if}

<style>
  :global(.group-duplicate-indicator) {
    color: var(--yellow);
    display: inline-flex;
    align-items: center;
    justify-content: center;
    padding: 0;
    flex: 0 0 auto;
  }

  :global(.group-duplicate-menu-trigger) {
    cursor: pointer;
    border-radius: 8px;
    transition: background-color 0.12s ease;
  }

  :global(.group-duplicate-menu-list) {
    display: flex;
    flex-direction: column;
    gap: 0.1rem;
    min-width: 360px;
    max-width: min(560px, calc(100vw - 1.2rem));
    max-height: min(70vh, 460px);
    overflow: auto;
  }

  :global(.duplicate-conflict-button) {
    width: 100%;
    align-items: center;
    gap: 0.5rem;
    padding: 0.4rem 0.5rem;
  }

  :global(.duplicate-conflict-button.active) {
    background-color: color-mix(in oklab, var(--yellow) 14%, transparent);
    border: 1px solid color-mix(in oklab, var(--yellow) 45%, transparent);
  }

  :global(.duplicate-conflict-label) {
    display: flex;
    flex-direction: column;
    align-items: flex-start;
    min-width: 0;
    gap: 0.2rem;
  }

  :global(.duplicate-conflict-title) {
    font-size: 0.95rem;
    width: 100%;
    color: var(--text);
    font-weight: 500;
    display: flex;
    align-items: center;
    gap: 0.4rem;
  }

  :global(.duplicate-conflict-name) {
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
    flex-shrink: 1;
  }

  :global(.duplicate-conflict-pattern-title) {
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
    flex-shrink: 1;
  }

  :global(.duplicate-conflict-subtitle) {
    display: flex;
    align-items: center;
    gap: 0.4rem;
    width: 100%;
  }

  :global(.duplicate-conflict-group) {
    font-size: 0.7rem;
    color: var(--text-2);
    background-color: var(--bg-light);
    border: 1px solid var(--bg-light-extra);
    padding: 0.1rem 0.3rem;
    border-radius: 0.25rem;
    white-space: nowrap;
    overflow: hidden;
    text-overflow: ellipsis;
    flex-shrink: 0;
    max-width: 120px;
    display: inline-flex;
    align-items: center;
    gap: 0.25rem;
  }

  :global(.group-color-dot) {
    width: 6px;
    height: 6px;
    border-radius: 50%;
    flex-shrink: 0;
  }

  :global(.duplicate-conflict-type) {
    font-size: 0.78rem;
    color: var(--text-2);
    white-space: nowrap;
    flex-shrink: 0;
  }

  :global(.duplicate-conflict-pattern) {
    font-size: 0.85rem;
    color: var(--text-2);
    white-space: nowrap;
    overflow: hidden;
    text-overflow: ellipsis;
    flex-shrink: 1;
  }

  :global(.duplicate-conflict-count) {
    margin-left: auto;
    font-size: 0.74rem;
    line-height: 1;
    color: var(--text);
    background-color: color-mix(in oklab, var(--yellow) 20%, transparent);
    border: 1px solid color-mix(in oklab, var(--yellow) 45%, transparent);
    border-radius: 999px;
    padding: 0.24rem 0.38rem;
    flex-shrink: 0;
  }
</style>
