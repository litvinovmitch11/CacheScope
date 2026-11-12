export interface Argument { index: number; shape: number[]; type: string }
export interface ProgramFunction { name: string; supported: boolean; reason: string; arguments: Argument[] }
export interface InputArray { key: string; shape: number[]; dtype: string }
export interface CacheLevel {
  name: string; size_bytes: number; line_size_bytes: number; associativity: number;
  replacement_policy: "LRU" | "FIFO" | "Random" | "PLRU4"; random_seed?: string;
  write_policy?: "write_back" | "write_through";
  allocation_policy?: "write_allocate" | "no_write_allocate";
}
export type InclusionPolicy = "exclusive" | "inclusive" | "non_inclusive";
export interface HierarchyConfig {
  inclusion_policies: InclusionPolicy[];
  writeback_clean: boolean;
  non_inclusive_fill: "all_levels" | "on_eviction";
}
export const defaultHierarchy = (count: number): HierarchyConfig => ({
  inclusion_policies: Array.from({ length: Math.max(0, count - 1) }, () => "exclusive"),
  writeback_clean: false, non_inclusive_fill: "all_levels",
});
export type Binding = { source: "npz"; key: string } | { source: "zeros" };
export interface RunSpec {
  function: string; bindings: Binding[]; levels: CacheLevel[];
  hierarchy?: HierarchyConfig; cache_profile?: string;
  timeout_seconds: number | null; trace_limit: string | null;
}
export interface Experiment {
  experiment_id: string;
  status: "preparing" | "ready" | "running" | "cancelling" | "completed" | "failed" | "cancelled";
  stage: string | null;
  metadata: { functions: ProgramFunction[]; arrays: InputArray[] } | null;
  spec: RunSpec | null;
  error: { code: string; message: string; diagnostic?: string } | null;
}
export interface Report { statistics: unknown; trace: unknown }
export const active = (job: Experiment | null) => !!job && ["preparing", "running", "cancelling"].includes(job.status);

export class ExperimentError extends Error {
  status: number;
  constructor(message: string, status: number) { super(message); this.status = status; }
}

export async function experimentRequest<T>(path = "", init?: RequestInit): Promise<T> {
  const response = await fetch("/api/v1/experiments" + path, init);
  const data = await response.json();
  if (!response.ok) throw new ExperimentError(data.error?.message ?? `Ошибка HTTP ${response.status}`, response.status);
  return data as T;
}
