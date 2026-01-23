import { token } from "../data/auth.svelte";
import { t } from "../data/locale.svelte";

import { toast } from "./events";

// @ts-ignore: vite specific
export const API_BASE = import.meta.env.DEV ? "http://localhost:6969/api/v1" : "/api/v1";

export async function fetcher<T>(...args: any[]): Promise<T> {
  const url = args.shift();
  const options = args[0] || {};

  if (token.current) {
    options.headers = {
      ...options.headers,
      Authorization: `Bearer ${token.current}`,
    };
  }

  if (args.length > 0) {
    args[0] = options;
  } else {
    args.push(options);
  }

  try {
    const res = await fetch(`${API_BASE}${url}`, ...args);

    if (res.status === 401 || res.status === 403) {
      token.reset();
      throw new Error("Unauthorized");
    }

    if (!res.ok || res.status < 200 || res.status > 299) {
      if (res.body) {
        throw new Error(await res.text());
      } else {
        throw new Error(res.statusText);
      }
    }
    return (await res.json()) as T;
  } catch (e) {
    console.error("Fetch error:", e);

    if ((e as Error).message !== "Unauthorized") {
      toast.error(t("Request failed"));
    }
    throw e;
  }
}

fetcher.get = <T>(url: string) =>
  fetcher<T>(url, {
    method: "GET",
  });

fetcher.post = <T>(url: string, body: any) =>
  fetcher<T>(url, {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify(body),
  });

fetcher.put = <T>(url: string, body: any) =>
  fetcher<T>(url, {
    method: "PUT",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify(body),
  });

fetcher.delete = <T>(url: string) =>
  fetcher<T>(url, {
    method: "DELETE",
  });
