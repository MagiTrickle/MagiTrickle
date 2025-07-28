<script lang="ts">
  import Select from "../common/Select.svelte";
  import { Locale, Gitlab, Bug } from "../common/icons";
  import { t, locale, locales } from "../../data/locale.svelte";
  const version = import.meta.env.VITE_PKG_VERSION || "0.0.0";
  const isDev = import.meta.env.VITE_PKG_VERSION_IS_DEV?.toLowerCase() === "true";
</script>

<div class="container">
  <div class="version">
    <span>build: {version}</span>
    {#if isDev}
      <div class="under-construction">dev</div>
    {/if}
  </div>
  <div class="links">
    <a
      target="_blank"
      rel="noopener noreferrer"
      href="https://gitlab.com/magitrickle/magitrickle/-/boards"><Bug size={22} /></a
    >
    <a target="_blank" rel="noopener noreferrer" href="https://gitlab.com/magitrickle/magitrickle"
      ><Gitlab size={22} /></a
    >
  </div>

  <div class="locale">
    <Locale size={18} />
    <Select
      options={Object.keys(locales).map((localeKey) => {
        switch (localeKey) {
          case "en":
            return { label: "en", value: localeKey };
          case "ru":
            return { label: "ру", value: localeKey };
          default:
            return { label: localeKey, value: localeKey };
        }
      })}
      bind:selected={locale.current}
      style="width:55px"
    />
  </div>
</div>

<style>
  .under-construction {
    background: repeating-linear-gradient(45deg, #ffcc00, #ffcc00 10px, #ff6600 10px, #ff6600 20px);
    color: black;
    font-weight: bold;
    padding: 4px 4px;
    border-radius: 4px;
    box-shadow: 0 4px 6px rgba(0, 0, 0, 0.1);
  }

  .container {
    display: flex;
    flex-direction: row;
    align-items: center;
    gap: 1.2rem;
  }

  .locale,
  .links,
  .version {
    display: flex;
    flex-direction: row;
    align-items: center;
    gap: 1rem;
  }

  .links a {
    & {
      color: var(--text);
      cursor: pointer;

      display: flex;
      align-items: center;
      justify-content: center;
    }

    &:hover {
      color: var(--accent);
    }
  }

  .version span {
    display: flex;
    align-items: center;
    justify-content: center;
    font-size: smaller;
    color: var(--text-2);
  }

  .locale {
    gap: 0.3rem;
    background: var(--bg-light);
    border-radius: 6px;
    padding: 2px 2px 2px 6px;
  }
</style>
