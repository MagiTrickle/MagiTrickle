import { type Interfaces } from "../types";
import { fetcher } from "../utils/fetcher";

export const interfaces = $state({
  list: [] as string[],
});

export async function fetchInterfaces() {
  try {
    const data = await fetcher.get<Interfaces>("/system/interfaces");
    interfaces.list = data.interfaces.map((item) => item.id);
  } catch (error) {
    console.error("Failed to fetch interfaces:", error);
    interfaces.list = [];
  }
}
