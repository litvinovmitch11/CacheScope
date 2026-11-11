import type {
  CreateSessionRequest,
  Progress,
  RunnerMetadata,
  RunnerSession,
  StateResponse,
  StepResponse,
} from "../types/runner";

export class RunnerHttpError extends Error {
  constructor(
    message: string,
    readonly status: number,
    readonly code?: string,
  ) {
    super(message);
    this.name = "RunnerHttpError";
  }
}

export class RunnerClient {
  readonly baseUrl: string;

  constructor(baseUrl: string) {
    this.baseUrl = baseUrl.replace(/\/+$/, "");
  }

  private async request<T>(path: string, init?: RequestInit): Promise<T> {
    let response: Response;
    try {
      response = await fetch(`${this.baseUrl}${path}`, {
        ...init,
        headers: { "Content-Type": "application/json", ...init?.headers },
      });
    } catch {
      throw new RunnerHttpError("Не удалось подключиться к backend. Проверьте адрес, запуск сервера и CORS.", 0);
    }

    if (!response.ok) {
      let message = `HTTP ${response.status}`;
      let code: string | undefined;
      try {
        const body = (await response.json()) as { error?: { message?: string; code?: string } };
        message = body.error?.message ?? message;
        code = body.error?.code;
      } catch {
        // Ошибка может не содержать JSON.
      }
      throw new RunnerHttpError(message, response.status, code);
    }
    if (response.status === 204) return undefined as T;
    return (await response.json()) as T;
  }

  createSession(body: CreateSessionRequest) {
    return this.request<RunnerSession>("/sessions", { method: "POST", body: JSON.stringify(body) });
  }

  listSessions() {
    return this.request<{ sessions: RunnerSession[] }>("/sessions");
  }

  getSession(id: string) {
    return this.request<RunnerSession>(`/sessions/${encodeURIComponent(id)}`);
  }

  getMetadata(id: string) {
    return this.request<RunnerMetadata>(`/sessions/${encodeURIComponent(id)}/metadata`);
  }

  step(id: string, revision: string) {
    return this.request<StepResponse>(`/sessions/${encodeURIComponent(id)}/step`, {
      method: "POST",
      body: JSON.stringify({ expected_revision: revision, count: 1 }),
    });
  }

  reset(id: string, revision: string) {
    return this.request<Progress>(`/sessions/${encodeURIComponent(id)}/reset`, {
      method: "POST",
      body: JSON.stringify({ expected_revision: revision }),
    });
  }

  getState(id: string, revision: string, level: number, setOffset: number, setLimit: number) {
    const query = new URLSearchParams({
      expected_revision: revision,
      level: String(level),
      set_offset: String(setOffset),
      set_limit: String(setLimit),
    });
    return this.request<StateResponse>(`/sessions/${encodeURIComponent(id)}/state?${query}`);
  }

  deleteSession(id: string, revision: string) {
    return this.request<void>(`/sessions/${encodeURIComponent(id)}?expected_revision=${encodeURIComponent(revision)}`, {
      method: "DELETE",
    });
  }
}
