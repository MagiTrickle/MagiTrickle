<script lang="ts">
  import { token } from "../data/auth.svelte";
  import { t } from "../data/locale.svelte";
  import Button from "./ui/Button.svelte";
  import InfoDialog from "./InfoDialog.svelte";

  import { toast } from "../utils/events";
  import { fetcher } from "../utils/fetcher";
  import { Info, Password, User } from "./ui/icons";

  let login = $state("");
  let password = $state("");
  let loading = $state(false);
  let error = $state(false);
  let isFocused = $state(false);
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

  function onFormFocusIn() {
    isFocused = true;
  }

  function onFormFocusOut(e: FocusEvent) {
    const next = e.relatedTarget as Node | null;
    if (next && (e.currentTarget as HTMLElement).contains(next)) return;
    isFocused = false;
  }
</script>

<div class="auth-page">
  <div class="left-panel">
    <div class="card">
      <form
        onsubmit={(e) => {
          e.preventDefault();
          submit();
        }}
        onfocusin={onFormFocusIn}
        onfocusout={onFormFocusOut}
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

  <div class="right-panel">
    <div class="brand-container">
      <div class="logo-wrapper" class:is-focused={isFocused}>
        <div class="logo-sticker" role="presentation">
          <img src="/static/logo.svg" alt="" class="logo-background" draggable="false" />
        </div>
      </div>
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
    align-items: center;
    justify-content: center;
    padding: 2rem 1rem;
    position: relative;
    z-index: 2;
  }

  .right-panel {
    position: absolute;
    inset: 0;
    display: block;
    overflow: hidden;
    z-index: 1;
    pointer-events: none;
  }

  .brand-container {
    position: absolute;
    top: 50%;
    left: 50%;
    transform: translate(-50%, -50%);
    width: 100%;
    height: 100%;
    display: grid;
    place-items: center;
    z-index: 1;
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
    position: absolute;
    top: 50%;
    left: 50%;
    transform: translate(-50%, -50%);
    width: clamp(12rem, 25vw, 20rem);
    z-index: 0;
    display: flex;
    justify-content: center;
    align-items: center;
    margin-top: -2.8rem;
    transition: margin-top 1.9s cubic-bezier(0.22, 1, 0.36, 1);
  }

  .logo-wrapper.is-focused {
    margin-top: -7.8rem;
  }

  .logo-sticker {
    width: 100%;
    height: 100%;
    position: relative;
    transform: rotate(-12deg) perspective(1000px) translateY(0) scale(0.98);
    transform-style: preserve-3d;
    pointer-events: none;
    transition: transform 1.8s cubic-bezier(0.22, 1, 0.36, 1);
  }

  .logo-wrapper.is-focused .logo-sticker {
    transform: rotate(-9deg) perspective(1000px) translateY(-0.7rem) scale(1.03);
    animation: logo-awake 7.8s ease-in-out infinite;
  }

  .logo-background {
    width: 100%;
    height: auto;
    display: block;
    opacity: 0.25;
    transform: translateZ(0);
    filter: grayscale(1) saturate(0.85) drop-shadow(0 2px 4px rgba(0, 0, 0, 0.12));
    transition: filter 1.5s ease, opacity 1.5s ease, transform 1.5s ease;
    user-select: none;
    -webkit-user-drag: none;
  }

  .logo-wrapper.is-focused .logo-background {
    opacity: 1;
    transform: translateZ(15px) scale(1.02);
    filter: grayscale(0) saturate(1.35) hue-rotate(-8deg)
      drop-shadow(0 10px 18px rgba(58, 122, 201, 0.38));
  }

  .logo-sticker::before,
  .logo-sticker::after {
    content: "";
    position: absolute;
    inset: 0;
    pointer-events: none;
    transition: opacity 1.1s ease, filter 1.5s ease;
    -webkit-mask-image: url('/static/logo.svg');
    -webkit-mask-size: contain;
    -webkit-mask-repeat: no-repeat;
    -webkit-mask-position: center;
    mask-image: url('/static/logo.svg');
    mask-size: contain;
    mask-repeat: no-repeat;
    mask-position: center;
    z-index: 2;
    filter: grayscale(1);
  }

  .logo-wrapper.is-focused .logo-sticker::before,
  .logo-wrapper.is-focused .logo-sticker::after {
    filter: grayscale(0);
  }

  .logo-sticker::before {
    opacity: 0;
    background:
      radial-gradient(
        circle at 42% 34%,
        rgba(255, 255, 255, 0.8) 0,
        rgba(90, 190, 255, 0.38) 16%,
        rgba(80, 255, 201, 0.2) 34%,
        transparent 50%
      );
    mix-blend-mode: color-dodge;
    transform: translateZ(2px);
  }

  .logo-sticker::after {
    opacity: 0;
    background:
      linear-gradient(
        118deg,
        transparent 20%,
        rgba(255, 105, 180, 0.4) 38%,
        rgba(238, 130, 238, 0.6) 45%,
        rgba(255, 255, 255, 0.9) 50%,
        rgba(238, 130, 238, 0.6) 55%,
        rgba(255, 105, 180, 0.4) 62%,
        transparent 80%
      );
    background-size: 230% 230%;
    background-position: 130% 50%;
    mix-blend-mode: hard-light;
    transform: translateZ(2px);
  }

  .logo-wrapper.is-focused .logo-sticker::before,
  .logo-wrapper.is-focused .logo-sticker::after {
    opacity: 1;
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

  @keyframes logo-awake {
    0%,
    100% {
      transform: rotate(-9deg) perspective(1000px) translateY(-0.7rem) scale(1.03);
    }

    50% {
      transform: rotate(-7.5deg) perspective(1000px) translateY(-1.2rem) scale(1.045);
    }
  }

  .logo-wrapper.is-focused .logo-sticker::before {
    animation: logo-glow 3s ease-in-out infinite;
  }

  .logo-wrapper.is-focused .logo-sticker::after {
    animation: logo-shimmer 2.8s linear infinite;
  }

  @keyframes logo-glow {
    0%,
    100% {
      opacity: 0.55;
    }

    50% {
      opacity: 0.95;
    }
  }

  @keyframes logo-shimmer {
    0% {
      background-position: 130% 50%;
    }

    100% {
      background-position: -30% 50%;
    }
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

    .right-panel {
      padding: 0;
    }

    .brand-container {
      position: absolute;
      top: 50%;
      left: 50%;
      transform: translate(-50%, -50%);
      width: 100%;
      height: 100%;
      justify-content: center;
      align-items: center;
    }

    .logo-wrapper {
      width: 12.6rem;
      margin-top: -7.2rem; /* В покое торчит только верхняя часть */
    }

    .logo-wrapper.is-focused {
      margin-top: -10.8rem; /* В активном состоянии видно примерно по пояс */
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
