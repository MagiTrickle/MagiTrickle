<script lang="ts">
  import { interfaces } from "../../data/interfaces.svelte";
  import { t } from "../../data/locale.svelte";
  import { profiles } from "../../data/profiles.svelte";
  import Select from "./Select.svelte";

  import {
    routeFromValue,
    routeValue,
    type RouteChoice,
  } from "../../modules/settings/profiles-data";

  type Props = {
    interface?: string;
    profile?: string;
    onChange?: (choice: RouteChoice) => void;
    ariaLabel?: string;
    disabled?: boolean;
    placeholder?: string;
    [key: string]: unknown;
  };
  let {
    interface: iface = $bindable(""),
    profile = $bindable<string | undefined>(),
    onChange,
    ariaLabel,
    disabled = false,
    placeholder = "",
    ...rest
  }: Props = $props();
  const selected = $derived(routeValue({ interface: iface, profile }));
  const options = $derived([
    ...profiles.list.map((item) => ({
      value: `profile/${item.id}`,
      label: item.name,
      group: t("Profiles"),
    })),
    ...interfaces.list.map((item) => ({
      value: `interface/${item.id}`,
      label: item.id,
      description: item.name,
      group: profiles.list.length ? t("Interfaces") : undefined,
    })),
  ]);

  function change(value: string) {
    const choice = routeFromValue(value, profiles.list);
    if (!choice) return;
    iface = choice.interface;
    profile = choice.profile;
    onChange?.(choice);
  }
</script>

<Select
  {options}
  {selected}
  {disabled}
  onValueChange={change}
  ariaLabel={ariaLabel ?? t("Route")}
  missingLabel={profile ? `${t("Missing profile")}: ${profile}` : iface || placeholder}
  compact={Boolean(profile)}
  {...rest}
/>
