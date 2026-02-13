<script lang="ts">
  import { token } from "../data/auth.svelte";
  import { t } from "../data/locale.svelte";
  import Button from "./ui/Button.svelte";
  import InfoDialog from "./InfoDialog.svelte";

  import { toast } from "../utils/events";
  import { fetcher } from "../utils/fetcher";
  import { Info, Password, User } from "./ui/icons";
  import logoUrl from "../assets/logo.svg";

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

<div class="auth-page">
  <div class="left-panel">
    <div class="logo-wrapper">
      <div class="logo-sticker">
        <img src={logoUrl} alt="" class="logo-background" draggable="false" />
      </div>
    </div>
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

  <button class="info-btn" title="Info" aria-label="Info" onclick={() => (infoIsOpen = true)}>
    <Info size={24} />
  </button>
</div>

<InfoDialog bind:open={infoIsOpen} />

<style>
  .auth-page {
    position: relative;
    display: flex;
    align-items: center;
    justify-content: center;
    min-height: 100vh;
    width: 100vw;
    box-sizing: border-box;
    overflow: hidden;
    background: linear-gradient(155deg, #10131d 0%, #111827 52%, #0f1420 100%);
  }

  .auth-page::before {
    content: "";
    position: absolute;
    inset: -22%;
    pointer-events: none;
    z-index: 0;
    background:
      radial-gradient(78rem 52rem at 8% 20%, rgba(66, 189, 249, 0.08) 0%, rgba(66, 189, 249, 0.03) 38%, transparent 74%),
      radial-gradient(72rem 56rem at 92% 82%, rgba(85, 158, 255, 0.07) 0%, rgba(85, 158, 255, 0.025) 40%, transparent 76%),
      radial-gradient(54rem 40rem at 52% 58%, rgba(11, 17, 30, 0.42) 0%, transparent 72%);
    filter: blur(16px);
    opacity: 0.72;
  }

  .auth-page::after {
    content: "";
    position: absolute;
    inset: 0;
    pointer-events: none;
    z-index: 0;
    background: radial-gradient(110% 95% at 50% 50%, transparent 58%, rgba(6, 9, 16, 0.28) 100%);
  }

  .left-panel {
    width: min(100%, 560px);
    display: flex;
    flex-direction: column;
    align-items: center;
    justify-content: center;
    gap: 0.75rem;
    padding: 2rem 1rem;
    position: relative;
    z-index: 2;
  }

  .info-btn {
    position: fixed;
    bottom: 20px;
    right: 20px;
    background: var(--bg-light);
    border: 1px solid var(--bg-light-extra);
    color: var(--text-2);
    border-radius: 50%;
    display: flex;
    align-items: center;
    justify-content: center;
    padding: 15px;
    cursor: pointer;
    z-index: 3;
    pointer-events: auto;
    box-shadow: 0 0 10px 2px var(--bg-dark-extra);
    outline: none;
    -webkit-tap-highlight-color: transparent;
  }

  .info-btn:hover {
    background: var(--bg-light-extra);
  }

  .info-btn:focus,
  .info-btn:focus-visible {
    outline: none;
    box-shadow: 0 0 10px 2px var(--bg-dark-extra);
  }

  .logo-wrapper {
    width: 128px;
    display: flex;
    justify-content: center;
    align-items: center;
    margin: 0 auto 1.25rem;
  }

  .logo-sticker {
    width: 100%;
    height: 100%;
    pointer-events: none;
  }

  .logo-background {
    width: 100%;
    height: auto;
    display: block;
    opacity: 1;
    filter: none;
    user-select: none;
    -webkit-user-drag: none;
  }

  .card {
    position: relative;
    z-index: 1;
    background-color: var(--bg-light);
    backdrop-filter: blur(8px);
    padding: 1.8rem 2rem 2rem;
    border-radius: 1rem;
    border: 1px solid var(--bg-light-extra);
    width: 100%;
    max-width: 420px;
    box-shadow: 0 12px 32px rgba(0, 0, 0, 0.3);
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
    transform: translateY(0.15rem);
    transition: opacity 0.45s ease, transform 0.45s ease;
  }

  .helper-text.visible {
    opacity: 1;
    transform: translateY(0);
  }

  .button-container {
    width: 33.333%;
  }

  @media (max-width: 700px) {
    .auth-page {
      align-items: center;
      justify-content: center;
    }

    .left-panel {
      padding: 1rem;
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
      border-radius: 0.8rem;
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
