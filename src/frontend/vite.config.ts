import { fileURLToPath } from "node:url";
import { svelte } from "@sveltejs/vite-plugin-svelte";
import { defineConfig } from "vite";
import { ViteMinifyPlugin } from "vite-plugin-minify";

export default defineConfig(() => ({
  publicDir: "static",
  resolve: {
    alias: {
      "#platform/settings": fileURLToPath(
        new URL(
          process.env.VITE_ENTWARE_KN === "1"
            ? "./src/modules/settings/keenetic/PolicyPanel.svelte"
            : "./src/modules/settings/PlatformSettings.empty.svelte",
          import.meta.url,
        ),
      ),
    },
  },
  plugins: [
    svelte({
      onwarn(warning, defaultHandler) {
        if (warning.code === "a11y_interactive_supports_focus") return;
        if (warning.code === "a11y_click_events_have_key_events") return;
        if (warning.code === "a11y_no_static_element_interactions") return;

        defaultHandler(warning);
      },
    }),
    ViteMinifyPlugin(),
  ],
  build: {
    // Reduce the gzip JavaScript footprint in router packages.
    minify: "terser" as const,
    cssTarget: "firefox115",
    emptyOutDir: true,
    target: "esnext",
    // assetsInlineLimit: 400000, // inline fonts
  },
}));
