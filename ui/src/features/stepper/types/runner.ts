export type UInt64 = string;

export interface LevelConfig {
  name: string;
  size_bytes: number;
  line_size_bytes: number;
  associativity: number;
  replacement_policy: "LRU" | "FIFO" | "Random";
  random_seed?: UInt64;
}

export interface EffectiveLevel extends Omit<LevelConfig, "random_seed"> {
  random_seed: string | null;
  level: number;
  set_count: number;
}

export interface SessionLimits {
  max_step_count: number;
  max_sets_per_read: number;
  max_instructions_per_read: number;
}

export interface RunnerSession {
  session_id: string;
  status: "preparing" | "ready" | "finished" | "failed";
  busy: boolean;
  cursor: UInt64;
  revision: UInt64;
  total_events: UInt64 | null;
  trace_path: string;
  levels: EffectiveLevel[];
  hierarchy: {
    inclusion_policy: "exclusive";
    write_policy: "write_back";
    allocation_policy: "write_allocate";
  };
  limits: SessionLimits;
  error: { code: string; message: string; recovery?: "reset" | "recreate" } | null;
}

export interface TraceEvent {
  sequence: UInt64;
  access_id: UInt64;
  memref_id: UInt64;
  byte_offset: string;
  size_bytes: UInt64;
  kind: "read" | "write";
  address_kind: "memref_offset";
}

export interface LineResult {
  line_address: string;
  hit: boolean;
  set: number;
  way: number | null;
  evicted_address: string | null;
  evicted_dirty: boolean | null;
}

export interface EventOutcome {
  trace_event: TraceEvent;
  address: string;
  level_results: Array<{ level: number; lines: LineResult[] }>;
  evictions: Array<{ level: number; line_address: string; dirty: boolean }>;
  memory_reads: UInt64;
  memory_writebacks: UInt64;
}

export interface Totals {
  read_line_accesses: UInt64;
  write_line_accesses: UInt64;
  hits: UInt64;
  misses: UInt64;
  read_hits: UInt64;
  read_misses: UInt64;
  write_hits: UInt64;
  write_misses: UInt64;
  evictions: UInt64;
  writebacks: UInt64;
  hit_rate_percent: number | null;
}

export interface Summary {
  processed: { trace_events: UInt64; line_accesses: UInt64 };
  totals: Totals;
  levels: Array<{ level: number; name: string; processed: { trace_events: UInt64; line_accesses: UInt64 }; totals: Totals }>;
  requests: { events: UInt64; reads: UInt64; writes: UInt64; read_bytes: UInt64; write_bytes: UInt64 };
  memory: { reads: UInt64; writebacks: UInt64 };
}

export interface Progress {
  session_id: string;
  status: "ready" | "finished";
  cursor: UInt64;
  revision: UInt64;
  summary: Summary;
  last_event: EventOutcome | null;
}

export interface StepResponse extends Progress {
  executed_count: number;
  events: EventOutcome[];
}

export interface CacheCell {
  way: number;
  valid: boolean;
  line_address: string | null;
  dirty: boolean;
}

export interface CacheSet {
  set: number;
  ways: CacheCell[];
}

export interface CachePage {
  level: number;
  set_offset: number;
  set_limit: number;
  total_sets: number;
  next_set_offset: number | null;
  sets: CacheSet[];
}

export interface StateResponse extends Progress {
  cache: CachePage;
}

export interface SourceLocation {
  stage_id: string;
  line: number;
  column: number;
  operation: string;
}

export interface SourceAccess {
  access_id: UInt64;
  memref_id: UInt64;
  kind: "read" | "write";
  address_kind: "memref_offset";
  source_location: string;
  origin_ids?: UInt64[];
  stage_locations?: SourceLocation[];
}

export interface SourceStage {
  stage_id: string;
  path: string;
}

export interface RunnerMetadata {
  session_id: string;
  trace_path: string;
  source: {
    source_schema: "cachescope.trace.v1";
    producer: Record<string, unknown>;
    variant: string;
    valid: true;
    complete: true;
    truncated: false;
    total_events: UInt64;
    recorded_events: UInt64;
    errors: string[];
    source_map?: {
      source_schema: "cachescope.source-map.v1";
      payload_fingerprint: string;
      stages: SourceStage[];
      origins: Array<{ origin_id: UInt64; stage_id: string; line: number; column: number; operation: string }>;
    };
  };
  buffers: Array<{ memref_id: UInt64; base_address: string; min_offset: string; end_offset: UInt64 }>;
  accesses: SourceAccess[];
}

export interface CreateSessionRequest {
  trace_path: string;
  levels: LevelConfig[];
}

export interface RunnerErrorBody {
  error?: { code?: string; message?: string; details?: unknown };
  session_id?: string;
  revision?: UInt64;
}
