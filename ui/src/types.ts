import { z } from "zod";

export const Totals = z.object({
  read_line_accesses: z.number().int(),
  write_line_accesses: z.number().int(),
  hits: z.number().int(),
  misses: z.number().int(),
  read_hits: z.number().int(),
  read_misses: z.number().int(),
  write_hits: z.number().int(),
  write_misses: z.number().int(),
  evictions: z.number().int(),
});

// Неизвестные поля zod по умолчанию отбрасывает — это требование спеки (v1).
export const Statistics = z.object({
  schema: z.literal("cachescope.simulation-statistics.v1"),
  variant: z.string(),
  producer: z.string(),
  cache_config: z.object({
    size_bytes: z.number().int(),
    line_size_bytes: z.number().int(),
    associativity: z.number().int(),
    set_count: z.number().int(),
    replacement_policy: z.string(),
  }),
  processed: z.object({
    trace_events: z.number().int(),
    line_accesses: z.number().int(),
  }),
  totals: Totals,
});

export type Statistics = z.infer<typeof Statistics>;
export type Totals = Statistics["totals"];

export interface Run {
  path: string; // например "torch-demo/simulation/l1-32k/statistics.json"
  stats: Statistics;
  warnings: string[]; // нарушенные инварианты
}

export interface LoadError {
  path: string;
  message: string;
}

export interface LoadResult {
  runs: Run[];
  errors: LoadError[];
}
