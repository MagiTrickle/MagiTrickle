<script lang="ts">
  import { t } from "../data/locale.svelte";
  import { fetcher } from "../utils/fetcher";
  import { token } from "../data/auth.svelte";
  import Button from "./ui/Button.svelte";
  import { toast } from "../utils/events";
  import { User, Password, Info } from "./ui/icons";

  let login = $state("");
  let password = $state("");
  let loading = $state(false);
  let error = $state(false);
  let isFocused = $state(false);

  let disabled = $derived(!login || !password || loading);

  async function submit() {
    if (disabled) return;
    loading = true;
    error = false;
    try {
      const res = await fetcher.post<{ token: string }>("/auth", { login, password });
      if (res.token) {
        token.current = res.token;
      }
    } catch (e) {
      console.error(e);
      toast.error(t("Login failed"));
      error = true;
      setTimeout(() => {
        error = false;
      }, 1000);
    } finally {
      loading = false;
    }
  }

  function onFocus() {
    isFocused = true;
  }
  function onBlur() {
    isFocused = false;
  }
</script>

<div class="auth-page">
  <div class="card">
    <form
      onsubmit={(e) => {
        e.preventDefault();
        submit();
      }}
    >
      <div class="field">
        <label for="login">{t("Login")}</label>
        <div class="input-wrapper">
          <span class="icon"><User size={18} /></span>
          <input
            id="login"
            type="text"
            bind:value={login}
            placeholder="..."
            onfocus={onFocus}
            onblur={onBlur}
          />
        </div>
      </div>
      <div class="field">
        <label for="password">{t("Password")}</label>
        <div class="input-wrapper">
          <span class="icon"><Password size={18} /></span>
          <input
            id="password"
            type="password"
            bind:value={password}
            placeholder="..."
            onfocus={onFocus}
            onblur={onBlur}
          />
        </div>
      </div>
      <div class="actions">
        <div class="helper-text" class:visible={isFocused}>
          <Info size={16} /><span> {t("entware credentials")}</span>
        </div>
        <div class="button-container">
          <Button
            class={error ? "fail" : ""}
            onclick={submit}
            {disabled}
            inactive={disabled}
            style="width: 100%"
          >
            {loading ? t("Loading...") : t("Sign In")}
          </Button>
        </div>
      </div>
    </form>
  </div>
</div>

<style>
  .auth-page {
    display: flex;
    align-items: center;
    justify-content: center;
    height: 100vh;
    width: 100vw;
    background-color: var(--bg-dark);
  }
  .card {
    background-color: var(--bg-light);
    padding: 2rem;
    border-radius: 8px;
    width: 100%;
    max-width: 400px;
    box-shadow: var(--shadow-popover);
    display: flex;
    flex-direction: column;
    margin: 0.5rem;
  }
  .field {
    margin-bottom: 1rem;
    display: flex;
    flex-direction: column;
    gap: 0.5rem;
  }
  label {
    color: var(--text-2);
    font-size: 0.9rem;
  }
  .input-wrapper {
    position: relative;
    display: flex;
    align-items: center;
  }
  .icon {
    position: absolute;
    left: 0.75rem;
    color: var(--text-2);
    display: flex;
    align-items: center;
    pointer-events: none;
  }
  input {
    background-color: var(--bg-dark-extra);
    border: 1px solid var(--bg-light-extra);
    color: var(--text);
    padding: 0.75rem;
    padding-left: 2.5rem;
    border-radius: 0.5rem;
    font-size: 1rem;
    font-family: var(--font);
    outline: none;
    transition: border-color 0.2s;
    width: 100%;
    box-sizing: border-box;
  }
  input::placeholder {
    font-family: var(--font);
    color: var(--text-2);
    opacity: 0.5;
  }
  input:focus {
    border-color: var(--accent);
  }
  .actions {
    margin-top: 1.5rem;
    display: flex;
    align-items: center;
    justify-content: space-between;
    gap: 1rem;
  }
  .helper-text {
    flex: 1;
    color: var(--text-2);
    font-size: 0.8rem;
    font-style: italic;
    display: flex;
    align-items: center;
    gap: 0.35rem;
    opacity: 0;
    transition: opacity 0.3s ease-in-out;
  }
  .helper-text.visible {
    opacity: 1;
  }
  .button-container {
    width: 33.333%;
  }
</style>
