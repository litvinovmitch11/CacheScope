import { useCallback, useEffect, useState } from "react";
import type { FormEvent } from "react";
import { active, defaultHierarchy, ExperimentError, experimentRequest } from "../experimentApi";
import type { Binding, CacheLevel, Experiment, HierarchyConfig, InclusionPolicy, Report, RunSpec } from "../experimentApi";
import { loadReport } from "../loadExperiment";
import { CACHE_PRESETS, findCachePreset } from "../cachePresets";
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
  const [hierarchy, setHierarchy] = useState<HierarchyConfig>(defaultHierarchy(1));
  const [cachePresetId, setCachePresetId] = useState("");
  const [timeout, setTimeoutValue] = useState("");
  const [traceLimit, setTraceLimit] = useState("");
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState("");

  const acceptJob = useCallback((value: Experiment | null) => {
    setJob(value);
    if (!value?.metadata) { setFunctionName(""); setBindings([]); return; }
    const spec = value.spec && "function" in value.spec ? value.spec : null;
    setFunctionName(spec?.function ?? value.metadata.functions.find(f => f.supported)?.name ?? "");
    setBindings(spec?.bindings.map(binding => binding.source === "zeros" ? "zeros" : "npz:" + binding.key) ?? []);
    if (spec) {
      setLevels(spec.levels);
      const policies = spec.hierarchy ?? defaultHierarchy(spec.levels.length);
      setHierarchy(policies);
      setCachePresetId(spec.cache_profile === "" ? "" : findCachePreset(spec.levels, policies, spec.cache_profile)?.id ?? "");
      setTimeoutValue(spec.timeout_seconds?.toString() ?? "");
      setTraceLimit(spec.trace_limit ?? "");
    }
  }, []);

  useEffect(() => {
    let stopped = false;
    experimentRequest<{ experiments: Experiment[] }>().then(data => {
      if (!stopped) acceptJob(data.experiments.find(item => !item.template_id) ?? null);
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
  const cachePreset = CACHE_PRESETS.find(preset => preset.id === cachePresetId);


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
        levels, hierarchy: { ...hierarchy, inclusion_policies: levels.slice(1).map((_, i) => hierarchy.inclusion_policies[i] ?? "exclusive") },
        cache_profile: cachePresetId, timeout_seconds: timeout === "" ? null : Number(timeout), trace_limit: traceLimit || null,
      };
      const value = await experimentRequest<Experiment>(`/${job.experiment_id}/run`, {
        method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify(spec),
      });
      onResult(null); acceptJob(value);
    });
  }
  function applyCachePreset(id: string) {
    setCachePresetId(id);
    const preset = CACHE_PRESETS.find(item => item.id === id);
    if (preset) {
      setLevels(preset.levels.map(level => ({ ...level })));
      setHierarchy({ ...preset.hierarchy, inclusion_policies: [...preset.hierarchy.inclusion_policies] });
    }
  }
  function changeLevels(update: (old: CacheLevel[]) => CacheLevel[]) {
    setCachePresetId("");
    const next = update(levels);
    setLevels(next);
    setHierarchy(h => ({ ...h, inclusion_policies: next.slice(1).map((_, i) => h.inclusion_policies[i] ?? "exclusive") }));
  }
  function changeHierarchy(change: Partial<HierarchyConfig>) {
    setCachePresetId(""); setHierarchy(old => ({ ...old, ...change }));
  }
  function changeLevel(index: number, change: Partial<CacheLevel>) {
    changeLevels(old => old.map((level, i) => {
      if (i !== index) return level;
      const updated = { ...level, ...change };
      if (updated.replacement_policy !== "Random") delete updated.random_seed;
      return updated;
    }));
  }

  return <section className="experiment-form">
    <h2>Запустить MLIR</h2>
    <p>Загрузите программу и NumPy-архив, затем явно сопоставьте каждый аргумент функции с массивом или нулевым буфером.</p>
    <details className="npz-help">
      <summary>Что находится в inputs.npz?</summary>
      <p>
        Это файл, созданный <code>numpy.savez</code>, например
        <code> np.savez("inputs.npz", source=array.astype(np.float32))</code>.
        Имя <code>source</code> станет вариантом в списке. UI разрешит выбрать
        его только для аргумента с таким же dtype <code>float32</code> и точно
        такой же статической формой.
      </p>
    </details>
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
        {CACHE_PRESETS.length > 0 && <label>Профиль процессора<select aria-label="Профиль процессора" aria-describedby="cache-preset-note" value={cachePresetId} onChange={e => applyCachePreset(e.target.value)}>
          <option value="">Ручная конфигурация</option>
          {CACHE_PRESETS.map(preset => <option key={preset.id} value={preset.id}>{preset.name}</option>)}
        </select></label>}
        <p className="muted cache-preset-note" id="cache-preset-note">
          {CACHE_PRESETS.length === 0
            ? "Подтверждённых профилей процессоров пока нет. Настройте кэши вручную."
            : "Профили содержат только подтверждённые настройки процессоров. Любое поле можно изменить вручную."}
          {" "}Размер строки должен совпадать на всех уровнях.
        </p>
        {cachePreset && <div className="muted cache-preset-note">
          <p>{cachePreset.description}</p>
          <ul>{cachePreset.sources.map(source => <li key={source.url}><a href={source.url} target="_blank" rel="noreferrer">{source.title}</a></li>)}</ul>
        </div>}
        {levels.map((level, i) => <div className="cache-level" key={i}>
          <strong>{level.name}</strong>
          <label>Размер, B<input aria-label={`${level.name} размер`} type="number" min="1" step="1" required value={level.size_bytes} onChange={e => changeLevel(i, { size_bytes: Number(e.target.value) })} /></label>
          <label>Строка, B<input aria-label={`${level.name} строка`} type="number" min="1" step="1" required value={level.line_size_bytes} onChange={e => changeLevel(i, { line_size_bytes: Number(e.target.value) })} /></label>
          <label>Ways<input aria-label={`${level.name} ways`} type="number" min={level.replacement_policy === "PLRU4" ? 4 : 1} max={level.replacement_policy === "PLRU4" ? 4 : undefined} step="1" required value={level.associativity} onChange={e => changeLevel(i, { associativity: Number(e.target.value) })} /></label>
          <label>Политика<select aria-label={`${level.name} политика`} value={level.replacement_policy} onChange={e => changeLevel(i, { replacement_policy: e.target.value as CacheLevel["replacement_policy"], ...(e.target.value === "PLRU4" ? { associativity: 4 } : {}) })}>
            <option>LRU</option><option>FIFO</option><option>Random</option><option value="PLRU4">PLRU (4-way)</option>
          </select></label>
          <label>Запись<select aria-label={`${level.name} запись`} value={level.write_policy ?? "write_back"} onChange={e => changeLevel(i, { write_policy: e.target.value as CacheLevel["write_policy"] })}>
            <option value="write_back">Write-back</option><option value="write_through">Write-through</option>
          </select></label>
          <label>Промах записи<select aria-label={`${level.name} размещение записи`} value={level.allocation_policy ?? "write_allocate"} onChange={e => changeLevel(i, { allocation_policy: e.target.value as CacheLevel["allocation_policy"] })}>
            <option value="write_allocate">Write-allocate</option><option value="no_write_allocate">No-write-allocate</option>
          </select></label>
          {level.replacement_policy === "Random" && <label>Seed<input aria-label={`${level.name} seed`} type="text" inputMode="numeric" pattern="[0-9]+" required value={level.random_seed ?? "0"} onChange={e => changeLevel(i, { random_seed: e.target.value })} /></label>}
        </div>)}
        <div className="hierarchy-policies">
          {levels.slice(1).map((level, i) => <label key={i}>{levels[i].name} ↔ {level.name}
            <select aria-label={`${levels[i].name}/${level.name} включение`} value={hierarchy.inclusion_policies[i] ?? "exclusive"} onChange={e => changeHierarchy({ inclusion_policies: levels.slice(1).map((_, link) => link === i ? e.target.value as InclusionPolicy : hierarchy.inclusion_policies[link] ?? "exclusive") })}>
              <option value="exclusive">Exclusive — одна копия</option>
              <option value="inclusive">Inclusive — верхний входит в нижний</option>
              <option value="non_inclusive">NINE — независимые копии</option>
            </select>
          </label>)}
          {levels.length > 1 && <>
            <label><input type="checkbox" aria-label="Передавать чистые вытеснения" checked={hierarchy.writeback_clean} onChange={e => changeHierarchy({ writeback_clean: e.target.checked })} />Передавать чистые вытеснения вниз (в exclusive включено всегда)</label>
            {hierarchy.inclusion_policies.includes("non_inclusive") && <label>Заполнение NINE<select aria-label="Заполнение NINE" value={hierarchy.non_inclusive_fill} onChange={e => changeHierarchy({ non_inclusive_fill: e.target.value as HierarchyConfig["non_inclusive_fill"] })}>
              <option value="all_levels">Все уровни на пути к найденной строке или памяти</option>
              <option value="on_eviction">Нижний уровень только при вытеснении сверху</option>
            </select></label>}
          </>}
          <p className="muted">Write-back сохраняет изменения в строке до передачи вниз. Write-through передаёт каждую запись дальше. No-write-allocate не загружает строку при промахе записи; чтения загружают её всегда. Inclusive-вытеснение из нижнего уровня удаляет копии сверху с сохранением dirty-данных.</p>
        </div>
        <button type="button" onClick={() => changeLevels(old => [...old, { ...firstLevel, name: `L${old.length + 1}`, size_bytes: old[old.length - 1].size_bytes * 4, line_size_bytes: old[0].line_size_bytes }])}>Добавить уровень</button>
        {levels.length > 1 && <button type="button" onClick={() => changeLevels(old => old.slice(0, -1))}>Удалить последний уровень</button>}
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
