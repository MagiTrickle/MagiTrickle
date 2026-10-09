import { svelte } from "@sveltejs/vite-plugin-svelte";
import { defineConfig } from "vite";
import { ViteMinifyPlugin } from "vite-plugin-minify";

export default defineConfig(() => ({
  publicDir: "static",
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
    cssTarget: "firefox115",
    emptyOutDir: true,
    target: "esnext",
    // assetsInlineLimit: 400000, // inline fonts
  },
}));
