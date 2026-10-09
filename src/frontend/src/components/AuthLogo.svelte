<script lang="ts">
  import logoUrl from "../../../../img/logo.svg";

  type Props = {
    anchor?: HTMLDivElement;
    illuminated?: boolean;
  };
  let { anchor = $bindable(), illuminated = false }: Props = $props();
</script>

<!-- Render the original vector at the browser's native resolution. WebGL only
     supplies the surrounding light; it never rasterizes the visible mark. -->
<div
  bind:this={anchor}
  class="logo-stage"
  class:illuminated
  aria-hidden="true"
  data-testid="auth-logo"
>
  <img src={logoUrl} alt="" draggable="false" />
</div>

<style>
  .logo-stage {
    position: relative;
    flex: none;
    width: 236px;
    height: 236px;
    margin: 0 auto 0.5rem;
    pointer-events: none;
  }
  img {
    display: block;
    width: 100%;
    height: 100%;
    filter: brightness(0.82) contrast(0.9) saturate(0.9);
    user-select: none;
  }
  .illuminated img {
    opacity: 0.9;
    mix-blend-mode: screen;
  }
  @media (max-width: 700px) {
    .logo-stage {
      width: 192px;
      height: 192px;
      margin-bottom: 0.95rem;
    }
  }
  @media (max-height: 660px) {
    .logo-stage {
      width: 140px;
      height: 140px;
      margin-bottom: 0.3rem;
    }
  }
</style>
