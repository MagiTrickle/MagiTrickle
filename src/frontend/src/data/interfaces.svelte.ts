import { type Interfaces } from "../types";
import { fetcher } from "../utils/fetcher";

export type InterfaceOption = Interfaces["interfaces"][number];

export const interfaces = $state({
  list: [] as InterfaceOption[],
});

export async function fetchInterfaces() {
  try {
    const data = await fetcher.get<Interfaces>("/system/interfaces");
    interfaces.list = data.interfaces;
  } catch (error) {
    console.error("Failed to fetch interfaces:", error);
    interfaces.list = [];
  }
}
