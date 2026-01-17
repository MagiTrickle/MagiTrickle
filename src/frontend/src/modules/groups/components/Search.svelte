<script lang="ts">
  import { t } from "../../../data/locale.svelte";
  import { Search } from "../../../components/ui/icons";

  type Props = {
    value: string;
    [key: string]: any;
  };

  let { value = $bindable(""), ...rest }: Props = $props();

  let isFocused = $state(false);
  let inputRef: HTMLInputElement;

  let isActive = $derived(isFocused || value.length > 0);

  function handleContainerClick() {
    inputRef?.focus();
  }
</script>

<div class="group-controls-search" {...rest}>
  <!-- svelte-ignore a11y_click_events_have_key_events -->
  <!-- svelte-ignore a11y_no_static_element_interactions -->
  <div class="search-container" class:active={isActive} onclick={handleContainerClick}>
    <span class="icon-wrapper">
      <Search />
    </span>

    <div class="input-wrapper">
      <input
        bind:this={inputRef}
        type="search"
        class="search-input"
        placeholder={isActive ? t("Search groups and rules...") : ""}
        bind:value
        onfocus={() => (isFocused = true)}
        onblur={() => (isFocused = false)}
      />
    </div>
  </div>
</div>

<style>
  .group-controls-search {
    display: flex;
    align-items: center;
    flex: 0 0 auto;
    transition: flex-grow 0.3s ease;
    min-width: 0;
  }

  .search-container {
    background-color: var(--bg-light);
    padding: 0.6rem;
    border: 1px solid var(--bg-light-extra);
    border-radius: 0.5rem;
    color: var(--text-2);
    font: 400 1rem var(--font);
    display: flex;
    align-items: center;
    cursor: pointer;
    box-sizing: border-box;
    white-space: nowrap;
    width: auto;
    transition:
      background-color 0.1s ease-in-out,
      border-color 0.1s ease-in-out,
      box-shadow 0.1s ease-in-out,
      color 0.1s ease-in-out;
  }

  .search-container:hover {
    background-color: var(--bg-light-extra);
    color: var(--text);
  }

  .search-container.active {
    cursor: text;
    background-color: var(--bg-light);
    color: var(--text);
    border-color: var(--accent);
    box-shadow:
      0 0 0 1px color-mix(in oklab, var(--accent) 45%, transparent),
      0 6px 18px -14px color-mix(in oklab, var(--accent) 35%, transparent);
  }

  .icon-wrapper {
    display: flex;
    align-items: center;
    justify-content: center;
    flex-shrink: 0;
  }

  .input-wrapper {
    width: 0;
    overflow: hidden;
    transition: width 0.3s cubic-bezier(0.25, 1, 0.5, 1);
  }

  .search-container.active .input-wrapper {
    margin-left: 0.3rem;
    width: 500px;
    max-width: 700px;
  }

  .search-input {
    appearance: none;
    border: none;
    background: transparent;
    outline: none;
    margin: 0;
    padding: 0;
    font: inherit;
    color: inherit;
    width: 100%;
    margin-left: 0.8rem;
    opacity: 0;
    transition: opacity 0.2s ease;
  }

  .search-container.active .search-input {
    opacity: 1;
  }

  @media (max-width: 700px) {
    .group-controls-search {
      flex: 1 1 auto;
    }

    .search-container.active {
      width: 100%;
    }

    .search-container.active .input-wrapper {
      width: 100vw;
    }
  }
</style>
