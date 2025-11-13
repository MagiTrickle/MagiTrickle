import { type Interfaces } from "../types";
import { fetcher } from "../utils/fetcher";

export const INTERFACES = $state<string[]>(
  await fetcher
    .get<Interfaces>("/system/interfaces")
    .then((data) => data.interfaces.map((item) => item.id))
    .catch((error) => {
      console.error(error);
      return [];
    })
);
