import { useCallback, useEffect, useRef, useState } from "react";
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

export interface StepPoint {
  sequence: number;
  hits: number;
  misses: number;
  neutral: boolean;
}
export type StepHistory = Record<number, StepPoint[]>;

export function useStepperSession(client: RunnerClient) {
  const [session, setSession] = useState<RunnerSession | null>(null);
  const [metadata, setMetadata] = useState<RunnerMetadata | null>(null);
  const [progress, setProgress] = useState<Progress | null>(null);
  const [lastStep, setLastStep] = useState<StepResponse | null>(null);
  const [cachePages, setCachePages] = useState<LevelSnapshot[]>([]);
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const [setStats, setSetStats] = useState<SetStats>({});
  const [stepHistory, setStepHistory] = useState<StepHistory>({});

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

  const accumulateStepHistory = useCallback(
    (events: EventOutcome[], levelIds: number[]) => {
      if (events.length === 0) return;
      setStepHistory((prev) => {
        const next: StepHistory = { ...prev };
        const adds: Record<number, StepPoint[]> = {};
        for (const ev of events) {
          const seq = Number(ev.trace_event.sequence);
          const seen = new Set<number>();
          for (const lr of ev.level_results) {
            seen.add(lr.level);
            let hits = 0;
            let misses = 0;
            for (const line of lr.lines) {
              if (line.hit) hits += 1;
              else misses += 1;
            }
            (adds[lr.level] ??= []).push({
              sequence: seq,
              hits,
              misses,
              neutral: false,
            });
          }
          for (const levelId of levelIds) {
            if (!seen.has(levelId)) {
              (adds[levelId] ??= []).push({
                sequence: seq,
                hits: 0,
                misses: 0,
                neutral: true,
              });
            }
          }
        }
        const HISTORY_LIMIT = 2000;
        for (const [level, items] of Object.entries(adds)) {
          const l = Number(level);
          const merged = [...(next[l] ?? []), ...items];
          next[l] = merged.length > HISTORY_LIMIT ? merged.slice(-HISTORY_LIMIT) : merged;
        }
        return next;
      });
    },
    [],
  );

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
      setStepHistory({});
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
      if (isBackendDown(cause)) handleBackendDown();
      else setError(cause instanceof Error ? cause.message : "Неизвестная ошибка");
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
      accumulateStepHistory(
        result.events,
        session.levels.map((l) => l.level),
      );
      const current = await client.getSession(session.session_id);
      setSession(current);
      await refreshState({ ...current, revision: result.revision });
    } catch (cause) {
      if (isBackendDown(cause)) {
        handleBackendDown();
      } else if (cause instanceof RunnerHttpError && cause.status === 409) {
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
      setProgress(result); setLastStep(null);
      setSetStats({});
      setStepHistory({});
      const current = await client.getSession(session.session_id);
      setSession(current);
      await refreshState({ ...current, revision: result.revision });
    } catch (cause) {
      if (isBackendDown(cause)) handleBackendDown();
      else setError(cause instanceof Error ? cause.message : "Не удалось сбросить сессию");
    } finally { setBusy(false); }
  }, [client, progress, refreshState, session]);

  const clearLocalState = useCallback(() => {
    setSession(null);
    setMetadata(null);
    setProgress(null);
    setLastStep(null);
    setCachePages([]);
    setSetStats({});
    setStepHistory({});
  }, []);

  const isBackendDown = (cause: unknown): boolean =>
    cause instanceof RunnerHttpError && cause.status === 0;

  const handleBackendDown = useCallback(() => {
    clearLocalState();
    setError("Backend недоступен — сессия завершена");
  }, [clearLocalState]);

  const clear = useCallback(async () => {
    if (session) {
      try { await client.deleteSession(session.session_id, session.revision); }
      catch (cause) {
        if (isBackendDown(cause)) {
          // бэкенд уже лежит — удалять нечего, просто очищаем локально
          clearLocalState();
          return;
        }
        setError(cause instanceof Error ? cause.message : "Не удалось удалить сессию");
        return;
      }
    }
    clearLocalState();
  }, [client, session, clearLocalState]);

  const heartbeatFailures = useRef(0);
  const activeSessionId = session?.session_id;

  useEffect(() => {
    heartbeatFailures.current = 0;
    if (!activeSessionId) return;
    const id = window.setInterval(async () => {
      try {
        await client.getSession(activeSessionId);
        heartbeatFailures.current = 0;
      } catch (cause) {
        if (isBackendDown(cause)) {
          heartbeatFailures.current += 1;
          if (heartbeatFailures.current >= 2) {
            handleBackendDown();
          }
        } else {
          heartbeatFailures.current = 0;
        }
      }
    }, 3000);
    return () => window.clearInterval(id);
  }, [client, activeSessionId, handleBackendDown]);

  return { session, metadata, progress, lastStep, cachePages, setStats, stepHistory, busy, error, create, step, reset, clear };
}
