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
  template_id?: string | null;
  status: "preparing" | "ready" | "running" | "cancelling" | "completed" | "failed" | "cancelled";
  stage: string | null;
  progress?: { stage: string; message?: string; current?: number; total?: number; variant?: string } | null;
  metadata: { functions: ProgramFunction[]; arrays: InputArray[] } | null;
  spec: RunSpec | TemplateRunSpec | TorchProjectSpec | TorchProjectDescriptorSpec | null;
  artifacts?: { report_url: string; summary_url: string; stepper_url: string } | null;
  error: { code: string; message: string; diagnostic?: string } | null;
}
export interface ExperimentTemplate {
  template_id: string;
  name: string;
  description: string;
  available: boolean;
  unavailable_reason: string | null;
  requirements: string[];
  parameters: {
    tile_sizes: { default: number[]; minimum: number; maximum: number; max_items: number };
    stepper_events: { default: number; minimum: number; maximum: number };
  };
}
export interface TemplateRunSpec {
  template_id: string;
  parameters: { tile_sizes: number[]; stepper_events: number };
  timeout_seconds: number | null;
}
export interface ProjectVariant { name: string; transform: string }
export type ProjectCacheProfile = string[] | {
  levels: CacheLevel[];
  hierarchy?: HierarchyConfig;
};
export interface TorchProjectSpec {
  schema: "cachescope.torch-project.v1";
  name: string;
  files: { "model.py": string; "inputs.py": string };
  variants: ProjectVariant[];
  cache_profiles: Record<string, ProjectCacheProfile>;
  settings: {
    stepper_events: number;
    trace_limit: number;
    validation: { rtol: number; atol: number };
  };
  timeout_seconds: number | null;
}
export interface TorchProjectDescriptorSpec {
  schema: "cachescope.torch-project.v1";
  name: string;
  variants: { name: string }[];
  cache_profiles: Record<string, ProjectCacheProfile>;
  settings: TorchProjectSpec["settings"];
  timeout_seconds: number | null;
}
export interface TemplateResults {
  entries: { path: string; data?: unknown; error?: string }[];
  variants?: Record<string, {
    status: string;
    error?: string;
    validation?: { torch_reference?: string; semantic_provenance?: string; rtol?: number; atol?: number };
  }>;
  contract?: {
    input_shapes: number[][];
    output_shape: number[];
    validation: { rtol: number; atol: number };
  };
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

export async function templateRequest(): Promise<{ templates: ExperimentTemplate[] }> {
  const response = await fetch("/api/v1/experiment-templates");
  const data = await response.json();
  if (!response.ok) throw new ExperimentError(data.error?.message ?? `Ошибка HTTP ${response.status}`, response.status);
  return data;
}
