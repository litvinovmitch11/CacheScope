import { z } from "zod";

const Int = z.number().int();
// Необязательные счётчики: null или отсутствие = «недоступно», а НЕ ноль.
const OptCount = Int.nullable().optional();

export const Totals = z.object({
  read_line_accesses: Int,
  write_line_accesses: Int,
  hits: Int,
  misses: Int,
  read_hits: Int,
  read_misses: Int,
  write_hits: Int,
  write_misses: Int,
  evictions: Int,
  writebacks: OptCount,
  back_invalidations: OptCount,
  hit_rate_percent: z.number().nullable().optional(), // справочно, UI не использует
});

export const CacheConfig = z.object({
  size_bytes: Int,
  line_size_bytes: Int,
  associativity: Int,
  set_count: Int,
  replacement_policy: z.string(),
  write_policy: z.string().optional(),
  allocation_policy: z.string().optional(),
});

export const Processed = z.object({
  trace_events: Int,
  line_accesses: Int,
});

export const LevelStats = z.object({
  level: Int,
  name: z.string(),
  cache_config: CacheConfig,
  processed: Processed,
  totals: Totals,
});

export const InstructionLevel = z.object({
  level: Int,
  processed: Processed,
  totals: Totals,
});

export const Instruction = z.object({
  access_id: Int,
  memref_id: Int,
  kind: z.string(),
  events: Int,
  requested_bytes: Int,
  levels: z.array(InstructionLevel),
});

// Неизвестные поля zod отбрасывает — это требование спеки (раздел 4).
export const Statistics = z.object({
  schema: z.literal("cachescope.simulation-statistics.v1"),
  variant: z.string(),
  producer: z.string(),
  // верхний уровень = представление L1
  cache_config: CacheConfig,
  processed: Processed,
  totals: Totals,
  // расширение для иерархии (раздел 6)
  levels: z.array(LevelStats).optional(),
  hierarchy: z
    .object({
      inclusion_policy: z.string().nullable().optional(),
      write_policy: z.string().nullable().optional(),
      allocation_policy: z.string().nullable().optional(),
      inclusion_policies: z.array(z.string()).optional(),
      writeback_clean: z.boolean().optional(),
      non_inclusive_fill: z.string().optional(),
    })
    .optional(),
  memory: z.object({ reads: OptCount, writes: OptCount, write_bytes: OptCount, writebacks: OptCount }).optional(),
  instructions: z.array(Instruction).optional(),
});

export type Statistics = z.infer<typeof Statistics>;
export type Totals = z.infer<typeof Totals>;
export type CacheConfig = z.infer<typeof CacheConfig>;
export type Processed = z.infer<typeof Processed>;
export type LevelStats = z.infer<typeof LevelStats>;
export type Instruction = z.infer<typeof Instruction>;

export const TraceSummary = z.object({
  schema: z.literal("cachescope.trace.v1"),
  variant: z.string(),
  total_events: Int,
  recorded_events: Int,
  truncated: z.boolean(),
  valid: z.boolean(),
  complete: z.boolean().optional(),
});

export type TraceSummary = z.infer<typeof TraceSummary>;
export type TraceHeader = TraceSummary;

export interface Run {
  path: string;
  stats: Statistics;
  trace: TraceSummary | null;
  warnings: string[];
}

export interface LoadError {
  path: string;
  message: string;
}

export interface LoadResult {
  runs: Run[];
  errors: LoadError[];
}
