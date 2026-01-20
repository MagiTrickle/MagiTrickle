import { Hono } from "hono";
import { serveStatic } from "hono/deno";
import { logger } from "hono/logger";
import { cors } from "hono/cors";
import { streamSSE } from "hono/streaming";
import type { Interfaces } from "../src/types.ts";

const API_BASE = "/api/v1";

const INTERFACES: Interfaces = {
  interfaces: [{ id: "nwg0" }, { id: "longinterf" }, { id: "eth1" }, { id: "wg0" }],
};

const DATA = JSON.parse(Deno.readTextFileSync("./dev/groups.json"));

function randomLogLine() {
  function randomIndex(array: any[]) {
    return array[Math.round(Math.random() * (array.length - 1))];
  }
  function randomIP(): string {
    return `${Math.round(Math.random() * 255)}.${Math.round(Math.random() * 255)}.${Math.round(
      Math.random() * 255,
    )}.${Math.round(Math.random() * 255)}`;
  }

  const level = randomIndex(["trace", "debug", "info", "warn", "error", "fatal", "panic"]);

  return {
    time: new Date().toISOString(),
    level: level,
    error: ["error", "fatal", "panic"].includes(level) ? "random error" : undefined,
    message: ["error", "fatal", "panic"].includes(level)
      ? "error message"
      : `group: ${randomIndex(DATA.groups).name}, ip: ${randomIP()} > int: ${randomIndex(
          INTERFACES.interfaces.map((item) => item.id),
        )}`,
  };
}

let sse_id = 0;

const PORT = 6969;
const STATIC_TOKEN = "magitrickle_mock_token_2026";
const AUTH_ENABLED = true;

const app = new Hono();

app.use(logger());
app.use(cors());

// Auth Middleware
app.use(`${API_BASE}/*`, async (c, next) => {
  if (c.req.path === `${API_BASE}/auth` || !AUTH_ENABLED) {
    await next();
    return;
  }

  const authHeader = c.req.header("Authorization");
  if (authHeader !== `Bearer ${STATIC_TOKEN}` && authHeader !== `Bearer disabled`) {
    return c.json({ error: "Unauthorized" }, 401);
  }

  await next();
});

app.get(`${API_BASE}/auth`, async (c) => {
  return c.json({ enabled: AUTH_ENABLED }, 200);
});

app.post(`${API_BASE}/auth`, async (c) => {
  const body = await c.req.json();
  if (body.login === "root" && body.password === "keenetic") {
    return c.json({ token: STATIC_TOKEN });
  }
  return c.json({ error: "Invalid credentials" }, 403);
});

app.get(`${API_BASE}/groups`, (c) => c.json(DATA));
app.put(`${API_BASE}/groups`, async (c) => {
  console.log("recieved", (await c.req.json())?.groups?.length, "groups");
  await new Promise((resolve) => setTimeout(resolve, 2000));
  if (Math.random() < 0.5) {
    return c.json({ error: "random error" }, 500);
  }
  return c.json({ status: "ok" });
});
app.get(`${API_BASE}/system/interfaces`, (c) => c.json(INTERFACES));
app.get(`${API_BASE}/logs`, async (c) => {
  return streamSSE(c, async (stream) => {
    while (true) {
      await stream.writeSSE({
        data: JSON.stringify(randomLogLine()),
        id: String(sse_id++),
      });
      await stream.sleep(Math.round(Math.random() * 1000));
    }
  });
});

app.get("*", serveStatic({ root: "./dist" }));

Deno.serve(
  { port: PORT, onListen: () => console.log(`running mock server on port ${PORT}...`) },
  app.fetch,
);
