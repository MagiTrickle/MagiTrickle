<script lang="ts">
  import { token } from "../data/auth.svelte";
  import { t } from "../data/locale.svelte";
  import AuroraScene from "./AuroraScene.svelte";
  import AuthLogo from "./AuthLogo.svelte";
  import InfoDialog from "./InfoDialog.svelte";
  import Button from "./ui/Button.svelte";

  import { toast } from "../utils/events";
  import { fetcher } from "../utils/fetcher";
  import { Info, Password, User } from "./ui/icons";

  let scene: HTMLDivElement | undefined = $state();
  let logoAnchor: HTMLDivElement | undefined = $state();
  let sceneReady = $state(false);

  let login = $state("");
  let password = $state("");
  let loading = $state(false);
  let error = $state(false);
  let infoIsOpen = $state(false);

  let disabled = $derived(!login || !password || loading);

  async function submit() {
    if (disabled) return;
    loading = true;
    error = false;
    try {
      const res = await fetcher.post<{ token?: string; error?: string }>("/auth", {
        login,
        password,
      });
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
</script>

<div class="auth-page" bind:this={scene}>
  <AuroraScene root={scene} anchor={logoAnchor} bind:ready={sceneReady} />
  <div class="left-panel">
    <AuthLogo bind:anchor={logoAnchor} illuminated={sceneReady} />
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
              autocomplete="username"
              bind:value={login}
              placeholder="..."
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
              autocomplete="current-password"
              bind:value={password}
              placeholder="..."
            />
          </div>
        </div>
        <div class="actions">
          <div class="helper-text visible">
            <Info size={16} /><span> {t("Entware account credentials")}</span>
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

  <div class="info-btn">
    <Button small title={t("About")} aria-label={t("About")} onclick={() => (infoIsOpen = true)}>
      <Info size={24} />
    </Button>
  </div>
</div>

<InfoDialog bind:open={infoIsOpen} />

<style>
  .auth-page {
    position: relative;
    isolation: isolate;
    display: flex;
    align-items: center;
    justify-content: center;
    min-height: 100vh;
    min-height: 100svh;
    width: 100%;
    box-sizing: border-box;
    overflow: hidden;
    background:
      radial-gradient(ellipse at 50% 30%, #0b2544 0%, transparent 48%),
      radial-gradient(ellipse at 75% 65%, #07324555, transparent 45%), #030914;
  }

  .left-panel {
    width: min(100%, 560px);
    display: flex;
    flex-direction: column;
    align-items: center;
    justify-content: center;
    gap: 0.75rem;
    padding: 3.5rem 1.5rem 4.5rem;
    box-sizing: border-box;
    position: relative;
    z-index: 2;
  }

  .info-btn {
    position: fixed;
    bottom: 20px;
    right: 20px;
    z-index: 3;
  }

  .card {
    position: relative;
    z-index: 1;
    background: linear-gradient(125deg, #a4cafa0d, #547ba905 48%, #82c7eb09), #0a142775;
    backdrop-filter: blur(20px);
    -webkit-backdrop-filter: blur(20px);
    padding: 2rem;
    box-sizing: border-box;
    border-radius: 1.3rem;
    border: 1px solid #b5d7f521;
    width: 100%;
    max-width: 480px;
    box-shadow:
      0 28px 70px #00000040,
      inset 0 1px 0 #dcf5ff0d,
      0 -12px 70px #167fff0a;
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
    color: #b5c6db;
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
    color: #b5c6db;
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
    color: #b5c6db;
    font-size: 0.8rem;
    font-style: italic;
    display: flex;
    align-items: center;
    gap: 0.35rem;
    opacity: 0;
    transform: translateY(0.15rem);
    transition:
      opacity 0.45s ease,
      transform 0.45s ease;
  }

  .helper-text.visible {
    opacity: 1;
    transform: translateY(0);
  }

  .button-container {
    width: 33.333%;
  }

  @media (prefers-reduced-motion: reduce) {
    input {
      transition: none;
    }
  }

  @media (max-width: 700px) {
    .auth-page {
      align-items: center;
      justify-content: center;
    }

    .left-panel {
      padding: 4.5rem 1.25rem 3rem;
      z-index: 2;
      width: 100%;
    }

    .info-btn {
      top: 1rem;
      right: 1rem;
      bottom: auto;
      pointer-events: auto;
    }

    .card {
      padding: 1.4rem 1.2rem 1.4rem;
      border-radius: 1.1rem;
    }

    .actions {
      flex-direction: column;
      align-items: stretch;
      gap: 0.8rem;
    }

    .button-container {
      width: 100%;
    }
  }
</style>
