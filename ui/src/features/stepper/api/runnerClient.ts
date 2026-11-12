import type {
  CreateSessionRequest,
  Progress,
  RunnerMetadata,
  RunnerSession,
  StateResponse,
  StepResponse,
  UploadResponse,
} from "../types/runner";

export interface RunnerLogEntry {
  id: string;
  time: string;
  method: string;
  url: string;
  status: number | null;
  duration: number;
  ok: boolean;
  response: string;
}

type LogCallback = (entry: RunnerLogEntry) => void;

export class RunnerHttpError extends Error {
  readonly status: number;
  readonly code?: string;

  constructor(message: string, status: number, code?: string) {
    super(message);
    this.name = "RunnerHttpError";
    this.status = status;
    this.code = code;
  }
}

export class RunnerClient {
  readonly baseUrl: string;
  private readonly onLog?: LogCallback;

  constructor(baseUrl: string, onLog?: LogCallback) {
    this.baseUrl = baseUrl.replace(/\/+$/, "");
    this.onLog = onLog;
  }

  private log(entry: RunnerLogEntry) {
    this.onLog?.(entry);
  }

  private async request<T>(path: string, init?: RequestInit): Promise<T> {
    const method = init?.method ?? "GET";
    const url = `${this.baseUrl}${path}`;
    const startedAt = Date.now();

    const isFormData = init?.body instanceof FormData;

    let response: Response;

    try {
      response = await fetch(url, {
        ...init,
        headers: isFormData
          ? init?.headers
          : {
            "Content-Type": "application/json",
            ...init?.headers,
          },
      });
    } catch (error) {
      const message =
        error instanceof Error
          ? `${error.name}: ${error.message}`
          : String(error);

      this.log({
        id: crypto.randomUUID(),
        time: new Date().toISOString(),
        method,
        url,
        status: null,
        duration: Date.now() - startedAt,
        ok: false,
        response: message,
      });

      throw new RunnerHttpError(
        `Не удалось подключиться к backend: ${message}. Проверьте адрес, запуск сервера и CORS.`,
        0,
      );
    }

    const duration = Date.now() - startedAt;

    if (response.status === 204) {
      this.log({
        id: crypto.randomUUID(),
        time: new Date().toISOString(),
        method,
        url,
        status: response.status,
        duration,
        ok: true,
        response: "No Content",
      });
      return undefined as T;
    }

    const rawBody = await response.text();

    if (!response.ok) {
      let message = `HTTP ${response.status}`;
      let code: string | undefined;

      try {
        const body = JSON.parse(rawBody) as {
          error?: { message?: string; code?: string };
        };
        message = body.error?.message ?? message;
        code = body.error?.code;
      } catch {
        // сохраняем исходное тело ответа
      }

      this.log({
        id: crypto.randomUUID(),
        time: new Date().toISOString(),
        method,
        url,
        status: response.status,
        duration,
        ok: false,
        response: rawBody || message,
      });

      throw new RunnerHttpError(message, response.status, code);
    }

    this.log({
      id: crypto.randomUUID(),
      time: new Date().toISOString(),
      method,
      url,
      status: response.status,
      duration,
      ok: true,
      response: "",
    });

    try {
      return JSON.parse(rawBody) as T;
    } catch {
      throw new RunnerHttpError(
        `Backend вернул некорректный JSON: ${rawBody.slice(0, 500)}`,
        response.status,
      );
    }
  }

  /** Загружает trace.json + опциональные MLIR-файлы на runner. */
  uploadArtifacts(files: File[]): Promise<UploadResponse> {
    const formData = new FormData();
    for (const file of files) {
      if (/trace\.json$/i.test(file.name)) {
        formData.append("trace", file);
      } else {
        formData.append("stages", file, file.name);
      }
    }
    return this.request<UploadResponse>("/uploads", {
      method: "POST",
      body: formData,
    });
  }

  /** Читает MLIR-файл через runner (аналог прежнего Vite-плагина). */
  async readSource(tracePath: string, stagePath: string): Promise<string> {
    const query = new URLSearchParams({
      trace_path: tracePath,
      stage_path: stagePath,
    });
    const result = await this.request<{ text: string }>(`/source?${query}`);
    return result.text;
  }

  createSession(body: CreateSessionRequest) {
    return this.request<RunnerSession>("/sessions", {
      method: "POST",
      body: JSON.stringify(body),
    });
  }

  listSessions() {
    return this.request<{ sessions: RunnerSession[] }>("/sessions");
  }

  getSession(id: string) {
    return this.request<RunnerSession>(`/sessions/${encodeURIComponent(id)}`);
  }

  getMetadata(id: string) {
    return this.request<RunnerMetadata>(
      `/sessions/${encodeURIComponent(id)}/metadata`,
    );
  }

  step(id: string, revision: string, count = 1) {
    return this.request<StepResponse>(
      `/sessions/${encodeURIComponent(id)}/step`,
      {
        method: "POST",
        body: JSON.stringify({ expected_revision: revision, count }),
      },
    );
  }

  reset(id: string, revision: string) {
    return this.request<Progress>(
      `/sessions/${encodeURIComponent(id)}/reset`,
      {
        method: "POST",
        body: JSON.stringify({ expected_revision: revision }),
      },
    );
  }

  getState(
    id: string,
    revision: string,
    level: number,
    setOffset: number,
    setLimit: number,
  ) {
    const query = new URLSearchParams({
      expected_revision: revision,
      level: String(level),
      set_offset: String(setOffset),
      set_limit: String(setLimit),
    });

    return this.request<StateResponse>(
      `/sessions/${encodeURIComponent(id)}/state?${query}`,
    );
  }

  deleteSession(id: string, revision: string) {
    return this.request<void>(
      `/sessions/${encodeURIComponent(id)}?expected_revision=${encodeURIComponent(revision)}`,
      { method: "DELETE" },
    );
  }
}
