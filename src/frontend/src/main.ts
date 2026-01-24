import { mount } from "svelte";

import "./app.css";
import "./assets/fonts.css";

import App from "./App.svelte";
import { initOverlayScrollbar } from "./utils/overlay-scrollbar";

const app = mount(App, { target: document.getElementById("app")! });

initOverlayScrollbar({ targetSelector: ".group-list" });

export default app;
