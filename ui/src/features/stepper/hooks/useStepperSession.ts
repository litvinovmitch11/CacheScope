import { useState, useCallback } from "react";
import { RunnerClient, RunnerHttpError } from "../api/runnerClient";
import type {
  CachePage,
  EventOutcome,
  Progress,
  RunnerMetadata,
  RunnerSession,
  StateResponse,
  StepResponse,
} from "../types/runner";

export interface LevelSnapshot {
  level: number;
  pages: CachePage[];
}

export interface SetStat {
  hits: number;
  misses: number;
}
export type SetStats = Record<number, Record<number, SetStat>>;

export function useStepperSession(client: RunnerClient) {
  const [session, setSession] = useState<RunnerSession | null>(null);
  const [metadata, setMetadata] = useState<RunnerMetadata | null>(null);
  const [progress, setProgress] = useState<Progress | null>(null);
  const [lastStep, setLastStep] = useState<StepResponse | null>(null);
  const [cachePages, setCachePages] = useState<LevelSnapshot[]>([]);
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const [setStats, setSetStats] = useState<SetStats>({});

  const accumulateSetStats = useCallback((events: EventOutcome[]) => {
    if (events.length === 0) return;
    setSetStats((prev) => {
      const next: SetStats = { ...prev };
      for (const ev of events) {
        for (const lr of ev.level_results) {
          const levelMap = { ...(next[lr.level] ?? {}) };
          for (const line of lr.lines) {
            const cur = levelMap[line.set] ?? { hits: 0, misses: 0 };
            levelMap[line.set] = {
              hits: cur.hits + (line.hit ? 1 : 0),
              misses: cur.misses + (line.hit ? 0 : 1),
            };
          }
          next[lr.level] = levelMap;
        }
      }
      return next;
    });
  }, []);

  const refreshState = useCallback(async (current: RunnerSession) => {
    const snapshots: LevelSnapshot[] = [];
    for (const level of current.levels) {
      const pages: CachePage[] = [];
      let offset = 0;
      do {
        const state: StateResponse = await client.getState(
          current.session_id,
          current.revision,
          level.level,
          offset,
          current.limits.max_sets_per_read,
        );
        pages.push(state.cache);
        offset = state.cache.next_set_offset ?? state.cache.total_sets;
        if (state.cache.next_set_offset === null) break;
      } while (offset < level.set_count);
      snapshots.push({ level: level.level, pages });
    }
    setCachePages(snapshots);
  }, [client]);

  const loadMetadata = useCallback(async (current: RunnerSession) => {
    const value = await client.getMetadata(current.session_id);
    setMetadata(value);
  }, [client]);

  const create = useCallback(async (tracePath: string, levels: Parameters<RunnerClient["createSession"]>[0]["levels"]) => {
    setBusy(true); setError(null);
    try {
      setSetStats({});
      let current = await client.createSession({ trace_path: tracePath, levels });
      setSession(current);
      while (current.status === "preparing") {
        await new Promise((resolve) => window.setTimeout(resolve, 400));
        current = await client.getSession(current.session_id);
        setSession(current);
      }
      if (current.status === "failed") throw new Error(current.error?.message ?? "Backend не смог подготовить trace");
      await loadMetadata(current);
      const state = await client.getState(current.session_id, current.revision, 1, 0, current.limits.max_sets_per_read);
      setProgress({
        session_id: state.session_id,
        status: state.status,
        cursor: state.cursor,
        revision: state.revision,
        summary: state.summary,
        last_event: state.last_event,
      });
      await refreshState(current);
    } catch (cause) {
      setError(cause instanceof Error ? cause.message : "Неизвестная ошибка");
    } finally { setBusy(false); }
  }, [client, loadMetadata, refreshState]);

  const step = useCallback(async (count = 1) => {
    if (!session || !progress || progress.status === "finished") return;
    if (count < 1 || count > session.limits.max_step_count) {
      setError(`count должен быть от 1 до ${session.limits.max_step_count}`);
      return;
    }
    setBusy(true); setError(null);
    try {
      const result = await client.step(session.session_id, progress.revision, count);
      setLastStep(result);
      setProgress(result);
      accumulateSetStats(result.events);
      const current = await client.getSession(session.session_id);
      setSession(current);
      await refreshState({ ...current, revision: result.revision });
    } catch (cause) {
      if (cause instanceof RunnerHttpError && cause.status === 409) {
        try {
          const current = await client.getSession(session.session_id);
          setSession(current);
          const recovered = await client.getState(current.session_id, current.revision, 1, 0, current.limits.max_sets_per_read);
          setProgress({ session_id: recovered.session_id, status: recovered.status, cursor: recovered.cursor, revision: recovered.revision, summary: recovered.summary, last_event: recovered.last_event });
          await refreshState(current);
          setError("Состояние сессии изменилось. Данные обновлены; повторите шаг, если он ещё нужен.");
        } catch (recoveryError) {
          setError(recoveryError instanceof Error ? recoveryError.message : "Не удалось восстановить состояние после конфликта");
        }
      } else setError(cause instanceof Error ? cause.message : "Ошибка выполнения шага");
    } finally { setBusy(false); }
  }, [client, progress, refreshState, session]);

  const reset = useCallback(async () => {
    if (!session || !progress) return;
    setBusy(true); setError(null);
    try {
      const result = await client.reset(session.session_id, progress.revision);
      setProgress(result); setLastStep(null);
      setSetStats({});
      const current = await client.getSession(session.session_id);
      setSession(current);
      await refreshState({ ...current, revision: result.revision });
    } catch (cause) {
      setError(cause instanceof Error ? cause.message : "Не удалось сбросить сессию");
    } finally { setBusy(false); }
  }, [client, progress, refreshState, session]);

  const clear = useCallback(async () => {
    if (session) {
      try { await client.deleteSession(session.session_id, session.revision); }
      catch (cause) { setError(cause instanceof Error ? cause.message : "Не удалось удалить сессию"); return; }
    }
    setSession(null); setMetadata(null); setProgress(null); setLastStep(null); setCachePages([]);
    setSetStats({});
  }, [client, session]);

  return { session, metadata, progress, lastStep, cachePages, setStats, busy, error, create, step, reset, clear };
}
