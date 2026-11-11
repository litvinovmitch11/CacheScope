import {
  Statistics,
  TraceSummary,
  type LoadError,
  type LoadResult,
  type Run,
} from "./types";
import { checkInvariants, checkTrace } from "./metrics";
import { readTraceHeader } from "./traceHeader";

const STATS_RE = /(?:^|\/)simulation\/.+\/statistics\.json$/;
const TRACE_RE = /(?:^|\/)trace\.json$/;
const isStats = (p: string) => STATS_RE.test(p);
// trace.json лежит вне simulation/: <exp>/<variant>/trace.json
const isTrace = (p: string) =>
  TRACE_RE.test(p) && !/(?:^|\/)simulation\//.test(p);

interface Item {
  path: string;
  json?: unknown;
  error?: string;
}

function issuesText(issues: { path: PropertyKey[]; message: string }[]) {
  return issues
    .map((i) => `${i.path.map(String).join(".") || "(корень)"}: ${i.message}`)
    .join("; ");
}

function sortRuns(runs: Run[]) {
  const ORDER = ["baseline", "tile4", "tile4-ikj"];
  const rank = (v: string) => {
    const i = ORDER.indexOf(v);
    return i === -1 ? ORDER.length : i;
  };
  runs.sort(
    (a, b) =>
      rank(a.stats.variant) - rank(b.stats.variant) ||
      a.path.localeCompare(b.path),
  );
}

function collect(items: Item[]): LoadResult {
  const errors: LoadError[] = [];
  const runs: Run[] = [];
  const traces = new Map<string, TraceSummary>();
  for (const it of items) {
    if (it.error) {
      errors.push({ path: it.path, message: it.error });
      continue;
    }
    if (!isTrace(it.path)) continue;
    const p = TraceSummary.safeParse(it.json);
    if (p.success) {
      traces.set(p.data.variant, p.data);
    } else {
      errors.push({
        path: it.path,
        message: issuesText(p.error.issues),
      });
    }
  }

  for (const it of items) {
    if (it.error || !isStats(it.path)) continue;
    const p = Statistics.safeParse(it.json);
    if (!p.success) {
      errors.push({ path: it.path, message: issuesText(p.error.issues) });
      continue;
    }
    const trace = traces.get(p.data.variant) ?? null;
    runs.push({
      path: it.path,
      stats: p.data,
      trace,
      warnings: [...checkInvariants(p.data), ...checkTrace(trace)],
    });
  }

  sortRuns(runs);
  return { runs, errors };
}

/** Выбор папки пользователем. */
export async function loadExperiment(files: FileList): Promise<LoadResult> {
  const picked = Array.from(files).filter(
    (f) => isStats(f.webkitRelativePath) || isTrace(f.webkitRelativePath),
  );

  const noStats: LoadError[] = picked.some((f) => isStats(f.webkitRelativePath))
    ? []
    : [
      {
        path: "(папка)",
        message:
          "Не найдено ни одного simulation/*/statistics.json. Выберите папку output/<experiment>/.",
      },
    ];

  const items: Item[] = await Promise.all(
    picked.map(async (f): Promise<Item> => {
      const path = f.webkitRelativePath;
      try {
        const json = isTrace(path)
          ? await readTraceHeader(f)
          : JSON.parse(await f.text());
        return { path, json };
      } catch (e) {
        return { path, error: e instanceof Error ? e.message : String(e) };
      }
    }),
  );

  const res = collect(items);
  return { runs: res.runs, errors: [...noStats, ...res.errors] };
}

/** Данные, вшитые в report.html скриптом make-report. В шаблоне там null. */
export function loadEmbedded(): LoadResult | null {
  const el = document.getElementById("cachescope-data");
  if (!el?.textContent) return null;
  let entries: unknown;
  try {
    entries = JSON.parse(el.textContent);
  } catch {
    return null;
  }
  if (!Array.isArray(entries)) return null;

  return collect(
    (entries as { path: string; data?: unknown; error?: string }[]).map((e) => ({
      path: e.path,
      json: e.data,
      error: e.error,
    })),
  );
}

/** The experiment API supplies the same artifact DTOs as a directory import. */
export function loadReport(report: { statistics: unknown; trace: unknown }): LoadResult {
  return collect([
    { path: "experiment/baseline/trace.json", json: report.trace },
    { path: "experiment/simulation/baseline/statistics.json", json: report.statistics },
  ]);
}
