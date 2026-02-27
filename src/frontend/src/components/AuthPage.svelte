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
  let stickerHover = $state(false);
  let stickerStyle = $state("--mx:50%; --my:50%; --rx:0deg; --ry:0deg; --lift:0px;");

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

  function onFocus() {
    isFocused = true;
  }
  function onBlur() {
    isFocused = false;
  }

  function onStickerMove(e: MouseEvent) {
    const target = e.currentTarget as HTMLElement | null;
    if (!target) return;
    const rect = target.getBoundingClientRect();
    const px = (e.clientX - rect.left) / rect.width;
    const py = (e.clientY - rect.top) / rect.height;
    const clampedX = Math.min(Math.max(px, 0), 1);
    const clampedY = Math.min(Math.max(py, 0), 1);
    const rotateY = (clampedX - 0.5) * 10;
    const rotateX = (0.5 - clampedY) * 10;
    const lift = 8 + Math.abs(rotateX) + Math.abs(rotateY) * 0.25;
    stickerStyle = `--mx:${(clampedX * 100).toFixed(1)}%; --my:${(clampedY * 100).toFixed(1)}%; --rx:${rotateX.toFixed(2)}deg; --ry:${rotateY.toFixed(2)}deg; --lift:${lift.toFixed(2)}px;`;
  }

  function onStickerEnter() {
    stickerHover = true;
  }

  function onStickerLeave() {
    stickerHover = false;
    stickerStyle = "--mx:50%; --my:50%; --rx:0deg; --ry:0deg; --lift:0px;";
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

  <div class="right-panel">
    <div class="brand-container">
      <div class="logo-wrapper">
        <div
          class="logo-sticker"
          role="presentation"
          class:is-hovered={stickerHover}
          style={stickerStyle}
          onmouseenter={onStickerEnter}
          onmouseleave={onStickerLeave}
          onmousemove={onStickerMove}
        >
          <img src="/static/logo.svg" alt="" class="logo-background" draggable="false" />
        </div>
      </div>
    </div>
    
    <button class="info-btn" title="Info" aria-label="Info" onclick={() => (infoIsOpen = true)}>
      <Info size={24} />
    </button>
  </div>
</div>

<InfoDialog bind:open={infoIsOpen} />

<style>
  .auth-page {
    position: relative;
    display: flex;
    flex-direction: row;
    min-height: 100vh;
    width: 100vw;
    box-sizing: border-box;
    overflow: hidden;
    background:
      radial-gradient(circle at 15% 20%, color-mix(in oklab, var(--accent) 10%, transparent) 0, transparent 36%),
      radial-gradient(circle at 80% 65%, color-mix(in oklab, var(--blue) 12%, transparent) 0, transparent 42%),
      var(--bg-dark);
  }

  .left-panel {
    flex: 1;
    display: flex;
    align-items: center;
    justify-content: center;
    padding: 2rem;
    position: relative;
    z-index: 1;
  }

  .right-panel {
    flex: 1;
    display: flex;
    flex-direction: column;
    align-items: center;
    justify-content: center;
    position: relative;
    overflow: hidden;
  }

  .brand-container {
    display: flex;
    flex-direction: column;
    align-items: center;
    gap: 2rem;
    z-index: 1;
  }

  .info-btn {
    position: absolute;
    bottom: 2rem;
    right: 2rem;
    background: var(--bg-light);
    border: 1px solid var(--bg-light-extra);
    color: var(--text-2);
    width: 3rem;
    height: 3rem;
    border-radius: 50%;
    display: flex;
    align-items: center;
    justify-content: center;
    cursor: pointer;
    transition: all 0.2s ease;
    z-index: 2;
  }

  .info-btn:hover {
    color: var(--text);
    border-color: var(--accent);
    transform: scale(1.05);
  }

  .logo-wrapper {
    position: relative;
    width: clamp(12rem, 25vw, 20rem);
    animation: drift 7s ease-in-out infinite;
    z-index: 0;
  }

  .logo-sticker {
    width: 100%;
    height: 100%;
    --mx: 50%;
    --my: 50%;
    --rx: 0deg;
    --ry: 0deg;
    --lift: 0px;
    transform: rotate(-12deg) perspective(1000px) rotateX(var(--rx)) rotateY(var(--ry)) translateY(calc(var(--lift) * -0.3));
    transform-style: preserve-3d;
    pointer-events: auto;
    cursor: pointer;
    transition: transform 0.22s ease;
  }

  .logo-background {
    width: 100%;
    height: auto;
    display: block;
    opacity: 0.9;
    transform: translateZ(0);
    filter: drop-shadow(0 4px 8px rgba(0, 0, 0, 0.2));
    transition: filter 0.22s ease, opacity 0.22s ease, transform 0.22s ease;
    user-select: none;
    -webkit-user-drag: none;
  }

  .logo-sticker::before,
  .logo-sticker::after {
    content: "";
    position: absolute;
    inset: 0;
    pointer-events: none;
    transition: opacity 0.2s ease;
    -webkit-mask-image: url('/static/logo.svg');
    -webkit-mask-size: contain;
    -webkit-mask-repeat: no-repeat;
    -webkit-mask-position: center;
    mask-image: url('/static/logo.svg');
    mask-size: contain;
    mask-repeat: no-repeat;
    mask-position: center;
    z-index: 2;
  }

  .logo-sticker::before {
    opacity: 0;
    background:
      radial-gradient(
        circle at var(--mx) var(--my),
        rgba(255, 255, 255, 0.8) 0,
        rgba(255, 182, 255, 0.4) 15%,
        rgba(138, 43, 226, 0.2) 30%,
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
    background-size: 200% 200%;
    background-position: calc((var(--mx) - 50%) * -2) calc((var(--my) - 50%) * -2);
    mix-blend-mode: hard-light;
    transform: translateZ(2px);
  }

  .logo-sticker.is-hovered .logo-background {
    opacity: 1;
    transform: translateZ(15px) scale(1.02);
    filter: drop-shadow(0 calc(8px + var(--lift) * 0.5) calc(12px + var(--lift) * 0.5) rgba(0, 0, 0, 0.3));
  }

  .logo-sticker.is-hovered::before,
  .logo-sticker.is-hovered::after {
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
    transition: opacity 0.3s ease-in-out;
  }

  .helper-text.visible {
    opacity: 1;
  }

  .button-container {
    width: 33.333%;
  }

  @keyframes drift {
    0%,
    100% {
      transform: translate3d(0, 0, 0);
    }

    50% {
      transform: translate3d(0, 10px, 0);
    }
  }

  @media (max-width: 700px) {
    .auth-page {
      flex-direction: column-reverse;
    }

    .left-panel {
      padding: 1rem;
    }

    .right-panel {
      padding: 2rem 1rem;
      flex: none;
    }

    .logo-wrapper {
      width: 12rem;
    }

    .info-btn {
      top: 1rem;
      right: 1rem;
      bottom: auto;
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
