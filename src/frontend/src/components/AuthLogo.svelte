<script lang="ts">
  import { onMount } from "svelte";

  import logoUrl from "../assets/auth-logo.png";

  let hidden = $state(false);

  onMount(() => {
    const updateVisibility = () => {
      hidden = document.hidden;
    };

    updateVisibility();
    document.addEventListener("visibilitychange", updateVisibility);
    return () => document.removeEventListener("visibilitychange", updateVisibility);
  });
</script>

<!-- A decorative brand mark; the login form remains the accessible content. -->
<div class="logo-stage" class:paused={hidden} aria-hidden="true" data-testid="auth-logo">
  <div class="logo-halo"></div>
  <div class="logo-float">
    <img src={logoUrl} alt="" draggable="false" />
    <div class="logo-sheen"></div>
  </div>
</div>

<style>
  .logo-stage {
    position: relative;
    isolation: isolate;
    flex: none;
    width: clamp(148px, 14vw, 186px);
    margin: 0 auto 0.75rem;
    pointer-events: none;
  }

  .logo-halo {
    position: absolute;
    z-index: -1;
    inset: 0 -20% 7%;
    border-radius: 50%;
    background: radial-gradient(
      ellipse at center,
      rgba(80, 166, 255, 0.23) 0%,
      rgba(56, 143, 235, 0.09) 48%,
      transparent 75%
    );
    filter: blur(24px);
    animation: logoBreath 9s ease-in-out infinite alternate;
  }

  .logo-float {
    position: relative;
    animation: logoFloat 8s ease-in-out infinite;
  }

  img {
    display: block;
    width: 100%;
    height: auto;
    user-select: none;
    -webkit-user-drag: none;
  }

  /* A slow highlight stays inside the original logo's alpha silhouette. */
  .logo-sheen {
    position: absolute;
    inset: 0;
    pointer-events: none;
    -webkit-mask-image: url("../assets/auth-logo.png");
    mask-image: url("../assets/auth-logo.png");
    -webkit-mask-repeat: no-repeat;
    mask-repeat: no-repeat;
    -webkit-mask-size: 100% 100%;
    mask-size: 100% 100%;
    background-image: linear-gradient(
      110deg,
      transparent 35%,
      rgba(200, 235, 255, 0) 42%,
      rgba(209, 245, 255, 0.32) 50%,
      rgba(200, 235, 255, 0) 58%,
      transparent 65%
    );
    background-repeat: no-repeat;
    background-size: 300% 100%;
    background-position: 100% 0;
    opacity: 0;
    animation: logoSheen 14s linear infinite;
  }

  @keyframes logoFloat {
    0%,
    100% {
      transform: translateY(0);
    }
    50% {
      transform: translateY(-6px);
    }
  }

  @keyframes logoBreath {
    from {
      opacity: 0.55;
      transform: scale(0.94);
    }
    to {
      opacity: 0.95;
      transform: scale(1.08);
    }
  }

  @keyframes logoSheen {
    0%,
    57% {
      background-position: 100% 0;
      opacity: 0;
    }
    62% {
      opacity: 0.9;
    }
    76% {
      background-position: 0% 0;
      opacity: 0.8;
    }
    82%,
    100% {
      background-position: 0% 0;
      opacity: 0;
    }
  }

  /* Keep all decorative movement out of the way while typing. */
  :global(.left-panel:focus-within) .logo-float,
  :global(.left-panel:focus-within) .logo-halo,
  :global(.left-panel:focus-within) .logo-sheen,
  .paused .logo-float,
  .paused .logo-halo,
  .paused .logo-sheen {
    animation-play-state: paused;
  }

  @media (max-width: 700px) {
    .logo-stage {
      width: clamp(122px, 35vw, 158px);
      margin-bottom: 0.4rem;
    }
  }

  @media (max-height: 660px) {
    .logo-stage {
      width: 120px;
      margin-bottom: 0;
    }
  }

  @media (prefers-reduced-motion: reduce) {
    .logo-float,
    .logo-halo,
    .logo-sheen {
      animation: none;
    }

    .logo-sheen {
      display: none;
    }

    .logo-halo {
      opacity: 0.6;
    }
  }
</style>
