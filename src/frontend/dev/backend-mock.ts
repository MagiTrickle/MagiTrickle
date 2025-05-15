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
      Math.random() * 255
    )}.${Math.round(Math.random() * 255)}`;
  }

  const fields = {
    time: new Date().toISOString(),
    ip: randomIP(),
    clientAddr: randomIP() + ":" + Math.round(Math.random() * 65535),
  };

  const level = randomIndex(["trace", "debug", "info", "warn", "error", "fatal", "panic"]);

  return {
    level: level,
    message: ["error", "fatal", "panic"].includes(level)
      ? "error message"
      : `group ${randomIndex(DATA.groups).name} int ${randomIndex(
          INTERFACES.interfaces.map((item) => item.id)
        )}`,
    fields: fields,
  };
}

let sse_id = 0;

const app = new Hono();

app.use(logger());
app.use(cors());

app.get(`${API_BASE}/groups`, (c) => c.json(DATA));
app.put(`${API_BASE}/groups`, async (c) => {
  console.log("recieved", (await c.req.json())?.groups?.length, "groups");
  await new Promise((resolve) => setTimeout(resolve, 2000));
  // throw new Error("random error");
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

const PORT = 6969;

Deno.serve(
  { port: PORT, onListen: () => console.log(`running mock server on port ${PORT}...`) },
  app.fetch
);
