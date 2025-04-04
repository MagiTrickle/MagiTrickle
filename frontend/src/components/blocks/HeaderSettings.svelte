<script lang="ts">
  import Select from "../common/Select.svelte";
  import { Locale, Github, Bug } from "../common/icons";
  import { t, locale, locales } from "../../data/locale.svelte";
  const version = import.meta.env.VITE_UPSTREAM_VERSION || "0.0.0";
  const isDev = import.meta.env.VITE_DEV?.toLowerCase() === "true";
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
      href="https://github.com/Ponywka/MagiTrickle/issues"><Bug size={22} /></a
    >
    <a target="_blank" rel="noopener noreferrer" href="https://github.com/Ponywka/MagiTrickle"
      ><Github size={22} /></a
    >
  </div>
  <!-- <div class="locale">
    <Locale size={22} />
    <Select
      options={Object.keys(locales).map((item) => ({ label: item, value: item }))}
      bind:selected={locale.current}
      style="width:55px"
    />
  </div> -->
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

  /* .locale, */
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
</style>
