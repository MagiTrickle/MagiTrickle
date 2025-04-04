<script lang="ts">
  import { scale } from "svelte/transition";
  import { onDestroy, onMount, untrack, tick } from "svelte";
  import { loaderState } from "svelte-infinite";

  import { parseConfig, type Group, type Rule } from "../../types";
  import { defaultGroup } from "../../utils/defaults";
  import { fetcher } from "../../utils/fetcher";
  import { overlay, toast } from "../../utils/events";
  import { persistedState } from "../../utils/persisted-state.svelte";
  import { INTERFACES } from "../../data/interfaces.svelte";
  import { Add, Upload, Download, Save } from "../common/icons";
  import Tooltip from "../common/Tooltip.svelte";
  import Button from "../common/Button.svelte";
  import GroupComponent from "../features/Group.svelte";

  const INITIAL_RULES_LIMIT = 30 as const;
  const INCREMENT_RULES_LIMIT = 40 as const;

  let data: Group[] = $state([]);
  let showed_limit: number[] = $state([]);
  let counter = $state(-2); // skip first update on init
  let valid_rules = $state(true);
  let open_state = persistedState<Record<string, boolean>>("group_open_state", {});

  function onRuleDrop(event: CustomEvent) {
    const { from_group_index, from_rule_index, to_group_index, to_rule_index } = event.detail;
    changeRuleIndex(from_group_index, from_rule_index, to_group_index, to_rule_index);
  }

  function unsavedChanges(event: BeforeUnloadEvent) {
    if (counter < 1) return;
    event.preventDefault();
  }

  function saveChanges() {
    if (counter === 0) return;
    overlay.show("saving changes...");

    fetcher
      .put("/groups?save=true", { groups: data })
      .then(() => {
        counter = 0;
        overlay.hide();
        toast.success("Saved");
      })
      .catch(() => {
        overlay.hide();
      });
  }

  function checkRulesValidityState() {
    valid_rules = !document.querySelector(".rule input.invalid");
  }

  function initOpenState() {
    for (const group of data) {
      if (!open_state.current[group.id]) {
        open_state.current[group.id] = false;
      }
    }
  }

  function cleanOrphanedOpenState() {
    for (const key of Object.keys(open_state.current)) {
      if (!data.some((group) => group.id === key)) {
        delete open_state.current[key];
      }
    }
  }

  onMount(async () => {
    data = (await fetcher.get<{ groups: Group[] }>("/groups?with_rules=true"))?.groups ?? [];
    showed_limit = data.map((group) =>
      group.rules.length > INITIAL_RULES_LIMIT ? INITIAL_RULES_LIMIT : group.rules.length,
    );
    initOpenState();
    setTimeout(cleanOrphanedOpenState, 5000);
    window.addEventListener("rule_drop", onRuleDrop);
  });

  onDestroy(() => {
    window.removeEventListener("rule_drop", onRuleDrop);
  });

  $effect(() => {
    const value = $state.snapshot(data);
    const new_count = untrack(() => counter) + 1;
    counter = new_count;
    if (new_count == 0) return;
    console.debug("config state", value, new_count);
    setTimeout(checkRulesValidityState, 10);
  });

  async function addRuleToGroup(group_index: number, rule: Rule, focus = false) {
    data[group_index].rules.unshift(rule);
    showed_limit[group_index]++;
    if (!focus) return;
    await tick();
    const el = document.querySelector(`.rule[data-group-index="${group_index}"][data-index="0"]`);
    el?.querySelector<HTMLInputElement>("div.name input")?.focus();
    el?.querySelector<HTMLInputElement>("div.pattern input")?.classList.add("invalid");
  }

  function deleteRuleFromGroup(group_index: number, rule_index: number) {
    data[group_index].rules.splice(rule_index, 1);
  }

  function changeRuleIndex(
    from_group_index: number,
    from_rule_index: number,
    to_group_index: number,
    to_rule_index: number,
  ) {
    const rule = data[from_group_index].rules[from_rule_index];
    data[from_group_index].rules.splice(from_rule_index, 1);
    data[to_group_index].rules.splice(to_rule_index, 0, rule);
    showed_limit[from_group_index]--;
    showed_limit[to_group_index]++;
  }

  async function addGroup() {
    data.unshift(defaultGroup());
    showed_limit.unshift(INITIAL_RULES_LIMIT);
    open_state.current[data[0].id] = false;
    await tick();
    const el = document.querySelector(`.group-header[data-group-index="0"]`);
    el?.querySelector<HTMLInputElement>("input.group-name")?.focus();
  }

  function deleteGroup(index: number) {
    data.splice(index, 1);
    showed_limit.splice(index, 1);
  }

  function groupMoveUp(index: number) {
    if (index === 0) return;
    data = [...data.slice(0, index - 1), data[index], data[index - 1], ...data.slice(index + 1)];
    showed_limit = [
      ...showed_limit.slice(0, index - 1),
      showed_limit[index],
      showed_limit[index - 1],
      ...showed_limit.slice(index + 1),
    ];
  }

  function groupMoveDown(index: number) {
    if (index === data.length - 1) return;
    data = [...data.slice(0, index), data[index + 1], data[index], ...data.slice(index + 2)];
    showed_limit = [
      ...showed_limit.slice(0, index),
      showed_limit[index + 1],
      showed_limit[index],
      ...showed_limit.slice(index + 2),
    ];
  }

  function exportConfig() {
    const blob = new Blob([JSON.stringify({ groups: data })], {
      type: "application/json",
    });
    const link = document.createElement("a");
    link.href = URL.createObjectURL(blob);
    link.download = "config.mtrickle";
    document.body.appendChild(link);
    link.click();
    document.body.removeChild(link);
  }

  function importConfig() {
    const input = document.getElementById("import-config") as HTMLInputElement;
    const file = input.files?.[0];

    console.debug("importing config", file?.name);
    if (file) {
      const reader = new FileReader();
      reader.onload = function (event) {
        try {
          let { groups } = parseConfig(event.target?.result as string);
          // for (let i = 0; i < groups.length; i++) {
          // if (!INTERFACES.includes(groups[i].interface)) {
          //   groups[i].interface = INTERFACES.at(0) ?? ""; // fallback to first interface
          // }
          // }
          data = groups;
          // TODO: eliminate mulitple groups iterations
          showed_limit = data.map((group) =>
            group.rules.length > INITIAL_RULES_LIMIT ? INITIAL_RULES_LIMIT : group.rules.length,
          );
          initOpenState();
          toast.success("Config imported");
        } catch (error) {
          console.error("Error parsing CONFIG:", error); // why is this not writing to console?
          toast.error("Invalid config file");
        }
      };
      reader.onerror = function (event) {
        console.error("Error reading file:", event.target?.error);
        toast.error("Invalid config file");
      };
      reader.readAsText(file);
      input.value = "";
    } else {
      alert("Please select a CONFIG file to load.");
    }
  }

  async function loadMore(group_index: number): Promise<void> {
    if (showed_limit[group_index] >= data[group_index].rules.length) return;
    showed_limit[group_index] += INCREMENT_RULES_LIMIT;
    if (showed_limit[group_index] > data[group_index].rules.length) {
      showed_limit[group_index] = data[group_index].rules.length;
      return;
    }
    loaderState.loaded();
  }
</script>

<svelte:window onbeforeunload={unsavedChanges} />

<div class="group-controls">
  <div class="group-controls-actions">
    {#if counter > 0 && valid_rules}
      <div transition:scale>
        <Tooltip value="Save Changes">
          <Button onclick={saveChanges} id="save-changes">
            <Save size={22} />
          </Button>
        </Tooltip>
      </div>
    {/if}
    <Tooltip value="Export Config">
      <Button onclick={exportConfig}>
        <Upload size={22} />
      </Button>
    </Tooltip>
    <Tooltip value="Import Config">
      <input type="file" id="import-config" hidden accept=".mtrickle" onchange={importConfig} />
      <Button onclick={() => document.getElementById("import-config")!.click()}>
        <Download size={22} />
      </Button>
    </Tooltip>
    <Tooltip value="Add Group">
      <Button onclick={addGroup}><Add size={22} /></Button>
    </Tooltip>
  </div>
</div>

{#each data as group, group_index (group.id)}
  <GroupComponent
    bind:group={data[group_index]}
    {group_index}
    bind:total_groups={data.length}
    bind:showed_limit={showed_limit[group_index]}
    bind:open={open_state.current[group.id]}
    {deleteGroup}
    {addRuleToGroup}
    {deleteRuleFromGroup}
    {changeRuleIndex}
    {groupMoveUp}
    {groupMoveDown}
    {loadMore}
  />
{/each}

<style>
  .group-controls {
    display: flex;
    align-items: end;
    justify-items: end;
    gap: 0.5rem;
    padding: 0.5rem 0 0.5rem 0;
    margin-bottom: 0.5rem;
  }

  .group-controls-actions {
    display: flex;
    align-items: end;
    justify-content: end;
    gap: 0.5rem;
    width: 100%;
  }
</style>
