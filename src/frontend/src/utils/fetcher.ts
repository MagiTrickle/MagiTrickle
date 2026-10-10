import { token } from "../data/auth.svelte";
import { t } from "../data/locale.svelte";

import { toast } from "./events";
import { HttpError } from "./http-error";

const viteEnv = (import.meta as ImportMeta & { env?: { DEV?: boolean } }).env;
export const API_BASE = viteEnv?.DEV ? "http://localhost:6969/api/v1" : "/api/v1";

export type FetchOptions = RequestInit & { silent?: boolean; timeoutMs?: number };

export async function fetcher<T>(url: string, settings: FetchOptions = {}): Promise<T> {
  const { silent = false, timeoutMs, ...options } = settings;
  if (token.current) {
    options.headers = {
      ...options.headers,
      Authorization: `Bearer ${token.current}`,
    };
  }

  const parentSignal = options.signal;
  const controller = timeoutMs ? new AbortController() : null;
  const abort = () => controller?.abort();
  let timer: ReturnType<typeof setTimeout> | undefined;
  if (controller) {
    options.signal = controller.signal;
    parentSignal?.addEventListener("abort", abort, { once: true });
    if (parentSignal?.aborted) controller.abort();
    timer = setTimeout(abort, timeoutMs);
  }

  try {
    const res = await fetch(`${API_BASE}${url}`, options);

    if (res.status === 401 || res.status === 403) {
      token.reset();
      throw new Error("Unauthorized");
    }

    if (!res.ok || res.status < 200 || res.status > 299) {
      if (res.body) {
        throw new HttpError(res.status, await res.text());
      } else {
        throw new HttpError(res.status, res.statusText);
      }
    }
    return (await res.json()) as T;
  } catch (e) {
    if (!silent) console.error("Fetch error:", e);

    if (!silent && (e as Error).message !== "Unauthorized") {
      let errorMessage = t("Request failed");
      try {
        const resBody = JSON.parse((e as Error).message);
        if (resBody?.error) {
          errorMessage = `${errorMessage}: ${resBody.error}`;
        }
      } catch {}
      toast.error(errorMessage);
    }
    throw e;
  } finally {
    if (timer) clearTimeout(timer);
    if (controller) parentSignal?.removeEventListener("abort", abort);
  }
}

fetcher.get = <T>(url: string, options: FetchOptions = {}) =>
  fetcher<T>(url, {
    ...options,
    method: "GET",
  });

fetcher.post = <T>(url: string, body: unknown) =>
  fetcher<T>(url, {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify(body),
  });

fetcher.put = <T>(url: string, body: unknown) =>
  fetcher<T>(url, {
    method: "PUT",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify(body),
  });

fetcher.patch = <T>(url: string, body: unknown) =>
  fetcher<T>(url, {
    method: "PATCH",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify(body),
  });

fetcher.delete = <T>(url: string) =>
  fetcher<T>(url, {
    method: "DELETE",
  });
