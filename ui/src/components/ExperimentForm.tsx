import { useCallback, useEffect, useState } from "react";
import type { FormEvent } from "react";
import { active, ExperimentError, experimentRequest } from "../experimentApi";
import type { Binding, CacheLevel, Experiment, Report, RunSpec } from "../experimentApi";
import { loadReport } from "../loadExperiment";
import type { LoadResult } from "../types";

const firstLevel: CacheLevel = { name: "L1", size_bytes: 32768, line_size_bytes: 64, associativity: 8, replacement_policy: "LRU" };
const stages: Record<string, string> = { inspection: "Проверка MLIR и входных данных", compilation: "Компиляция", execution: "Исполнение и запись трассы", simulation: "Моделирование кэшей" };
const statuses: Record<Experiment["status"], string> = { preparing: "Подготовка", ready: "Готов к запуску", running: "Выполняется", cancelling: "Отмена…", completed: "Готово", failed: "Ошибка", cancelled: "Отменён" };

export function ExperimentForm({ onResult }: { onResult: (result: LoadResult | null) => void }) {
  const [job, setJob] = useState<Experiment | null>(null);
  const [mlir, setMlir] = useState<File | null>(null);
  const [npz, setNpz] = useState<File | null>(null);
  const [functionName, setFunctionName] = useState("");
  const [bindings, setBindings] = useState<string[]>([]);
  const [levels, setLevels] = useState<CacheLevel[]>([firstLevel]);
  const [timeout, setTimeoutValue] = useState("");
  const [traceLimit, setTraceLimit] = useState("");
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState("");

  const acceptJob = useCallback((value: Experiment | null) => {
    setJob(value);
    if (!value?.metadata) { setFunctionName(""); setBindings([]); return; }
    setFunctionName(value.spec?.function ?? value.metadata.functions.find(f => f.supported)?.name ?? "");
    setBindings(value.spec?.bindings.map(binding => binding.source === "zeros" ? "zeros" : "npz:" + binding.key) ?? []);
    if (value.spec) {
      setLevels(value.spec.levels);
      setTimeoutValue(value.spec.timeout_seconds?.toString() ?? "");
      setTraceLimit(value.spec.trace_limit ?? "");
    }
  }, []);

  useEffect(() => {
    let stopped = false;
    experimentRequest<{ experiments: Experiment[] }>().then(data => {
      if (!stopped) acceptJob(data.experiments[0] ?? null);
    }).catch(e => { if (!stopped) setError(`Сервис запуска недоступен: ${e.message}`); });
    return () => { stopped = true; };
  }, [acceptJob]);

  const id = job?.experiment_id;
  const status = job?.status;
  useEffect(() => {
    if (!id || !status || !["preparing", "running", "cancelling"].includes(status)) return;
    let stopped = false;
    let timer: number;
    async function poll() {
      try {
        const value = await experimentRequest<Experiment>("/" + id);
        if (!stopped) { acceptJob(value); setError(""); }
      } catch (e) {
        if (!stopped) {
          if (e instanceof ExperimentError && e.status === 404) {
            stopped = true;
            acceptJob(null);
          }
          setError(e instanceof Error ? e.message : String(e));
        }
      }
      if (!stopped) timer = window.setTimeout(() => void poll(), 500);
    }
    timer = window.setTimeout(() => void poll(), 500);
    return () => { stopped = true; window.clearTimeout(timer); };
  }, [id, status, acceptJob]);

  useEffect(() => {
    if (!id || status !== "completed") return;
    let stopped = false;
    experimentRequest<Report>(`/${id}/report`).then(report => {
      if (!stopped) onResult(loadReport(report));
    }).catch(e => { if (!stopped) setError(e.message); });
    return () => { stopped = true; };
  }, [id, status, onResult]);

  const metadata = job?.metadata;


  const selected = metadata?.functions.find(f => f.name === functionName);
  const running = active(job);
  async function action(operation: () => Promise<void>) {
    setBusy(true); setError("");
    try { await operation(); } catch (e) { setError(e instanceof Error ? e.message : String(e)); }
    finally { setBusy(false); }
  }
  function upload(event: FormEvent) {
    event.preventDefault();
    if (!mlir || !npz) return;
    void action(async () => {
      const files = new FormData(); files.append("mlir", mlir); files.append("inputs", npz);
      const value = await experimentRequest<Experiment>("", { method: "POST", body: files });
      onResult(null); acceptJob(value);
    });
  }
  function run(event: FormEvent) {
    event.preventDefault();
    if (!job || !selected) return;
    void action(async () => {
      const spec: RunSpec = {
        function: functionName,
        bindings: selected.arguments.map((_, i): Binding => bindings[i] === "zeros" ? { source: "zeros" } : { source: "npz", key: bindings[i].slice(4) }),
        levels, timeout_seconds: timeout === "" ? null : Number(timeout), trace_limit: traceLimit || null,
      };
      const value = await experimentRequest<Experiment>(`/${job.experiment_id}/run`, {
        method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify(spec),
      });
      onResult(null); acceptJob(value);
    });
  }
  function changeLevel(index: number, change: Partial<CacheLevel>) {
    setLevels(old => old.map((level, i) => {
      if (i !== index) return level;
      const updated = { ...level, ...change };
      if (updated.replacement_policy !== "Random") delete updated.random_seed;
      return updated;
    }));
  }

  return <section className="experiment-form">
    <h2>Запустить MLIR</h2>
    <p>Загрузите программу и NPZ, выберите функцию и источники её аргументов.</p>
    <form onSubmit={upload}>
      <label>MLIR-файл<input aria-label="MLIR-файл" type="file" accept=".mlir" required disabled={running || busy} onChange={e => setMlir(e.target.files?.[0] ?? null)} /></label>
      <label>Входные данные (NPZ)<input aria-label="Входные данные (NPZ)" type="file" accept=".npz" required disabled={running || busy} onChange={e => setNpz(e.target.files?.[0] ?? null)} /></label>
      <button type="submit" disabled={running || busy || !mlir || !npz}>{busy && !job ? "Загрузка…" : "Проверить файлы"}</button>
    </form>
    {job && <div className="experiment-progress" role="status">
      <strong>{statuses[job.status]}</strong>{job.stage && <span> — {stages[job.stage] ?? job.stage}</span>}
      {running && <button type="button" disabled={busy || job.status === "cancelling"} onClick={() => void action(async () => {
        acceptJob(await experimentRequest<Experiment>(`/${job.experiment_id}/cancel`, { method: "POST" }));
      })}>Отменить</button>}
    </div>}
    {(error || job?.error) && <div className="errors" role="alert">
      <p>{error || job?.error?.message}</p>
      {job?.error?.diagnostic && <details><summary>Диагностика</summary><pre>{job.error.diagnostic}</pre></details>}
    </div>}
    {metadata && <form onSubmit={run}>
      <fieldset disabled={job?.status !== "ready" || busy}>
        <legend>Программа и аргументы</legend>
        <label>Функция<select aria-label="Функция" value={functionName} onChange={e => { setFunctionName(e.target.value); setBindings([]); }}>
          {metadata.functions.map(f => <option key={f.name} value={f.name} disabled={!f.supported}>{f.name}{!f.supported ? ` — ${f.reason}` : ""}</option>)}
        </select></label>
        {selected?.arguments.map((argument, i) => <label key={functionName + i}>
          Аргумент {i}: <code>{argument.type}</code>
          <select aria-label={`Аргумент ${i}`} required value={bindings[i] ?? ""} onChange={e => setBindings(old => { const next = [...old]; next[i] = e.target.value; return next; })}>
            <option value="" disabled>Выберите источник</option>
            <option value="zeros">Заполнить нулями</option>
            {metadata.arrays.map(array => <option key={array.key} value={"npz:" + array.key} disabled={array.dtype !== "float32" || JSON.stringify(array.shape) !== JSON.stringify(argument.shape)}>
              {array.key}: {array.dtype} [{array.shape.join(" × ")}]
            </option>)}
          </select>
        </label>)}
      </fieldset>
      <fieldset disabled={job?.status !== "ready" || busy}>
        <legend>Иерархия кэша</legend>
        {levels.map((level, i) => <div className="cache-level" key={i}>
          <strong>{level.name}</strong>
          <label>Размер, B<input aria-label={`${level.name} размер`} type="number" min="1" step="1" required value={level.size_bytes} onChange={e => changeLevel(i, { size_bytes: Number(e.target.value) })} /></label>
          <label>Строка, B<input aria-label={`${level.name} строка`} type="number" min="1" step="1" required value={level.line_size_bytes} onChange={e => changeLevel(i, { line_size_bytes: Number(e.target.value) })} /></label>
          <label>Ways<input aria-label={`${level.name} ways`} type="number" min="1" step="1" required value={level.associativity} onChange={e => changeLevel(i, { associativity: Number(e.target.value) })} /></label>
          <label>Политика<select aria-label={`${level.name} политика`} value={level.replacement_policy} onChange={e => changeLevel(i, { replacement_policy: e.target.value as CacheLevel["replacement_policy"] })}>
            <option>LRU</option><option>FIFO</option><option>Random</option>
          </select></label>
          {level.replacement_policy === "Random" && <label>Seed<input aria-label={`${level.name} seed`} type="text" inputMode="numeric" pattern="[0-9]+" required value={level.random_seed ?? "0"} onChange={e => changeLevel(i, { random_seed: e.target.value })} /></label>}
        </div>)}
        <button type="button" onClick={() => setLevels(old => [...old, { ...firstLevel, name: `L${old.length + 1}`, size_bytes: old[old.length - 1].size_bytes * 4, line_size_bytes: old[0].line_size_bytes }])}>Добавить уровень</button>
        {levels.length > 1 && <button type="button" onClick={() => setLevels(old => old.slice(0, -1))}>Удалить последний уровень</button>}
      </fieldset>
      <fieldset disabled={job?.status !== "ready" || busy}>
        <legend>Ограничения по желанию</legend>
        <p>Пустые поля означают выполнение без ограничений, до завершения или ручной отмены.</p>
        <label>Время, секунд<input aria-label="Время, секунд" type="number" min="0.001" step="any" value={timeout} onChange={e => setTimeoutValue(e.target.value)} /></label>
        <label>Число событий<input aria-label="Число событий" type="text" inputMode="numeric" pattern="[1-9][0-9]*" value={traceLimit} onChange={e => setTraceLimit(e.target.value)} /></label>
      </fieldset>
      <button type="submit" disabled={job?.status !== "ready" || busy || !selected || selected.arguments.some((_, i) => !bindings[i])}>Выполнить</button>
      {job?.status === "completed" && <p>Для нового запуска выберите файлы и нажмите «Проверить файлы».</p>}
    </form>}
  </section>;
}
