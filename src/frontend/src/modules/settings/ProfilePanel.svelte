<script lang="ts">
  import { Collapsible } from "bits-ui";
  import { tick } from "svelte";
  import { slide } from "svelte/transition";

  import Button from "../../components/ui/Button.svelte";
  import Select from "../../components/ui/Select.svelte";
  import Tooltip from "../../components/ui/Tooltip.svelte";
  import { interfaces } from "../../data/interfaces.svelte";
  import { t } from "../../data/locale.svelte";

  import {
    Add,
    Delete,
    Grip,
    GroupCollapse,
    GroupExpand,
    Link,
    Lock,
  } from "../../components/ui/icons";
  import { draggable, droppable } from "../../lib/dnd";
  import type { Profile } from "../../types";
  import { moveInterface } from "./profiles-data";

  type Props = {
    profile: Profile;
    usage?: Profile["usage"];
    disabled: boolean;
    ondelete: () => void;
  };
  type InterfaceDrag = { profile: string; name: string; edge?: "before" | "after" };
  let { profile, usage, disabled, ondelete }: Props = $props();
  let open = $state(true);
  let candidate = $state("");
  let dropEdge = $state<"before" | "after">("before");
  const used = $derived(Boolean(usage && (usage.groups || usage.subscriptions)));
  const available = $derived(
    interfaces.list.filter(
      (item) => item.id !== "blackhole" && !profile.interfaces.includes(item.id),
    ),
  );

  async function addInterface(value: string) {
    if (!disabled && available.some((item) => item.id === value)) {
      profile.interfaces.push(value);
      open = true;
    }
    // The picker is an add action, not the last selected interface. Reset it so
    // the same interface can be added again after removal.
    await tick();
    candidate = "";
  }

  function updateDropIntent(event: DragEvent) {
    const row = event.currentTarget as HTMLElement;
    const rect = row.getBoundingClientRect();
    dropEdge = event.clientY - rect.top > rect.height / 2 ? "after" : "before";
  }

  function drop(source: InterfaceDrag, target: InterfaceDrag) {
    if (disabled || source.profile !== profile.id || target.profile !== profile.id) return;
    const from = profile.interfaces.indexOf(source.name);
    const at = profile.interfaces.indexOf(target.name);
    if (from < 0 || at < 0 || from === at) return;
    const insert = at + (target.edge === "after" ? 1 : 0);
    moveInterface(profile, from, insert - (from < insert ? 1 : 0));
  }

  function keyboardMove(event: KeyboardEvent, index: number) {
    if (disabled || !event.altKey || !["ArrowUp", "ArrowDown"].includes(event.key)) return;
    event.preventDefault();
    moveInterface(profile, index, index + (event.key === "ArrowUp" ? -1 : 1));
  }
</script>

<article class="profile" data-profile-id={profile.id}>
  <Collapsible.Root bind:open>
    <div class="profile-header">
      <div class="profile-info">
        <input
          type="text"
          class="profile-name"
          bind:value={profile.name}
          placeholder={t("Profile name")}
          aria-label={t("Profile name")}
          maxlength={256}
        />
        {#if usage}
          <div class="usage" title={t("Used by")}>
            <Link size={14} aria-hidden="true" />
            <span>{usage.groups} {t("groups")}, {usage.subscriptions} {t("subscriptions")}</span>
          </div>
        {/if}
      </div>
      <div class="profile-actions">
        <div
          class="add-interface"
          title={available.length ? t("Add interface") : t("No more interfaces available")}
        >
          <Select
            ariaLabel={t("Add interface")}
            bind:selected={candidate}
            disabled={disabled || !available.length}
            options={available.map((item) => ({
              value: item.id,
              label: item.id,
              description: item.name,
            }))}
            onValueChange={addInterface}
          >
            {#snippet trigger()}
              <Add size={20} /><span class="add-label">{t("Add interface")}</span>
            {/snippet}
          </Select>
        </div>
        <Tooltip
          value={used
            ? t("Reassign groups and subscriptions before deleting this profile")
            : t("Delete profile")}
        >
          <Button
            small
            disabled={disabled || used}
            inactive={disabled || used}
            onclick={ondelete}
            aria-label={`${t("Delete profile")}: ${profile.name}`}
          >
            <Delete size={20} />
          </Button>
        </Tooltip>
        <Tooltip value={t(open ? "Collapse" : "Expand")}>
          <Collapsible.Trigger {disabled} aria-label={t(open ? "Collapse" : "Expand")}>
            {#if open}<GroupCollapse size={20} />{:else}<GroupExpand size={20} />{/if}
          </Collapsible.Trigger>
        </Tooltip>
      </div>
    </div>
    <Collapsible.Content>
      <div transition:slide>
        <div class="interface-table" role="table" aria-label={profile.name || t("Profile")}>
          <div class="interface-header" role="row">
            <div role="columnheader"></div>
            <div role="columnheader">#</div>
            <div role="columnheader">{t("Interface")}</div>
            <div role="columnheader" aria-label={t("Actions")}></div>
          </div>
          {#each profile.interfaces as name, index (name)}
            {@const description = interfaces.list.find((item) => item.id === name)?.name}
            <div
              class="interface"
              role="row"
              data-interface={name}
              data-drop-edge={dropEdge}
              use:draggable={{
                data: { profile: profile.id, name } as InterfaceDrag,
                scope: "profile-interface",
                handle: ".grip",
                effects: { effectAllowed: "move", dropEffect: "move" },
                dragImage: (node) => {
                  const preview = node.cloneNode(true) as HTMLElement;
                  preview.style.cssText = `position:fixed;top:-1000px;left:-1000px;width:${node.getBoundingClientRect().width}px;pointer-events:none;`;
                  return preview;
                },
              }}
              use:droppable={{
                data: { profile: profile.id, name, edge: dropEdge } as InterfaceDrag,
                scope: "profile-interface",
                canDrop: (source: InterfaceDrag) =>
                  !disabled && source.profile === profile.id && source.name !== name,
                dropEffect: "move",
                onDrop: drop,
              }}
            >
              <div
                class="interface-row"
                role="presentation"
                ondragenter={updateDropIntent}
                ondragover={updateDropIntent}
              >
                <div role="cell">
                  <button
                    type="button"
                    class="grip"
                    {disabled}
                    title={t("Drag interface (Alt+↑/↓)")}
                    aria-label={`${t("Drag interface")}: ${name}`}
                    onkeydown={(event) => keyboardMove(event, index)}><Grip size={18} /></button
                  >
                </div>
                <div class="order" role="cell">{index + 1}</div>
                <div class="interface-label" role="cell">
                  <span class="interface-id">{name}</span>
                  {#if description}<span class="interface-description" title={description}
                      >{description}</span
                    >{/if}
                </div>
                <div class="row-actions" role="cell">
                  <Tooltip value={t("Remove interface")}>
                    <Button
                      small
                      {disabled}
                      onclick={() => profile.interfaces.splice(index, 1)}
                      aria-label={`${t("Remove interface")}: ${name}`}><Delete size={20} /></Button
                    >
                  </Tooltip>
                </div>
              </div>
            </div>
          {/each}
          <div class="interface terminal" role="row" aria-disabled="true">
            <div class="interface-row" role="presentation">
              <div class="terminal-icon" role="cell"><Lock size={16} /></div>
              <div class="order" role="cell"></div>
              <div class="interface-label" role="cell">blackhole</div>
              <div role="cell"></div>
            </div>
          </div>
        </div>
      </div>
    </Collapsible.Content>
  </Collapsible.Root>
</article>

<style>
  /* Same panel, title and striped rows as SubscriptionPanel/SubscriptionRuleRow. */
  .profile {
    background-color: var(--bg-medium);
    border-radius: 0.5rem;
    border: 1px solid var(--bg-light-extra);
    margin: 1rem 0;
  }
  .profile:first-child {
    margin-top: 0;
  }
  .profile-header {
    display: flex;
    justify-content: space-between;
    align-items: center;
    gap: 0.5rem;
    padding: 0.5rem;
    border-radius: 0.5rem;
    background-color: var(--bg-light);
  }
  .profile-info {
    display: flex;
    flex-direction: column;
    flex: 1;
    min-width: 0;
    gap: 0.2rem;
  }
  .profile-name {
    box-sizing: border-box;
    border: none;
    border-bottom: 1px solid transparent;
    background-color: transparent;
    font: 600 1.3rem var(--font);
    color: var(--text);
    width: 100%;
    min-width: 0;
    padding-left: 0.4rem;
  }
  .profile-name:focus-visible {
    outline: none;
    border-bottom-color: var(--accent);
  }
  .usage {
    display: flex;
    align-items: center;
    gap: 0.4rem;
    font-size: 0.8rem;
    color: var(--text-2);
    margin-left: 0.4rem;
  }
  .profile-actions,
  .row-actions {
    display: flex;
    align-items: center;
    justify-content: center;
    gap: 0.35rem;
    flex-shrink: 0;
  }
  .row-actions {
    justify-content: flex-end;
  }
  .interface-header,
  .interface-row {
    display: grid;
    grid-template-columns: 2rem 1.75rem minmax(0, 1fr) 2.5rem;
    gap: 0.25rem;
    align-items: center;
    padding: 0.1rem 0.1rem 0.1rem 0;
  }
  .interface-header {
    font-size: 0.9rem;
    color: var(--text-2);
    text-align: center;
    padding-top: 0.6rem;
    padding-bottom: 0.2rem;
    border-bottom: 1px solid var(--bg-light-extra);
  }
  .interface:nth-child(even) {
    background-color: var(--bg-light);
  }
  .interface-row {
    min-height: 2rem;
  }
  .interface-label {
    display: flex;
    align-items: baseline;
    gap: 0.5rem;
    min-width: 0;
    padding: 0 0.5rem;
  }
  .interface-id {
    flex-shrink: 0;
  }
  .interface-description {
    color: var(--text-2);
    font-size: 0.9rem;
    white-space: nowrap;
    overflow: hidden;
    text-overflow: ellipsis;
  }
  .order {
    text-align: center;
    font-size: 0.9rem;
    color: var(--text-2);
  }
  .grip,
  .terminal-icon {
    display: flex;
    align-items: center;
    justify-content: center;
    width: 100%;
    color: var(--text-2);
  }
  .grip {
    border: none;
    background: transparent;
    cursor: grab;
    padding: 0.4rem 0;
    user-select: none;
    -webkit-user-select: none;
    -webkit-user-drag: none;
  }
  .grip:hover {
    color: var(--text);
  }
  .interface:global(.dragover)[data-drop-edge="before"] {
    box-shadow: inset 0 2px var(--accent);
  }
  .interface:global(.dragover)[data-drop-edge="after"] {
    box-shadow: inset 0 -2px var(--accent);
  }
  .interface:global(.dragging) {
    opacity: 0.4;
  }
  .terminal .interface-row {
    color: var(--text-2);
    opacity: 0.55;
  }
  .add-interface {
    display: flex;
    align-items: center;
  }
  .add-interface :global(.select-wrap) {
    display: flex;
    align-items: center;
  }
  .add-interface :global([data-select-trigger]) {
    justify-content: center;
    padding: 0.4rem;
    border: 1px solid transparent;
    color: var(--text-2);
    gap: 0.3rem;
  }
  .add-interface :global([data-select-trigger]:hover) {
    color: var(--text);
  }
  .add-interface :global([data-select-trigger]:disabled) {
    opacity: 0.3;
    cursor: default;
  }
  @media (max-width: 700px) {
    .add-label {
      display: none;
    }
    .interface-header {
      height: 1px;
      padding: 0;
      border: none;
    }
    .interface-header > div {
      display: none;
    }
    .interface-row {
      padding: 0.35rem 0.35rem 0.35rem 0;
    }
    .interface-label {
      flex-direction: column;
      gap: 0.1rem;
      padding: 0;
    }
    .interface-description {
      max-width: 100%;
      font-size: 0.8rem;
    }
    .order {
      font-size: 0.8rem;
    }
    .profile-name {
      font-size: 1.3rem;
    }
  }
</style>
