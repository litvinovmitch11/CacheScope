import { readFile, stat, readdir, open } from "node:fs/promises";
import { homedir } from "node:os";
import path from "node:path";
import type { Plugin } from "vite";

const TRACE_HEADER_BYTES = 64 * 1024;
const MAX_DIR_ENTRIES = 2000;

async function looksLikeTrace(filePath: string): Promise<boolean> {
  try {
    const handle = await open(filePath, "r");
    try {
      const buffer = Buffer.alloc(TRACE_HEADER_BYTES);
      const { bytesRead } = await handle.read(buffer, 0, TRACE_HEADER_BYTES, 0);
      return /"schema"\s*:\s*"cachescope\.trace\.v1"/.test(
        buffer.toString("utf8", 0, bytesRead),
      );
    } finally {
      await handle.close();
    }
  } catch {
    return false;
  }
}

export function localFilesPlugin(): Plugin {
  return {
    name: "cachescope-stepper-local-files",
    apply: "serve",
    configureServer(server) {
      server.middlewares.use("/__stepper/read-stage", async (req, res) => {
        if (req.method !== "POST") {
          res.statusCode = 405;
          res.end("Method not allowed");
          return;
        }

        let raw = "";
        req.on("data", (chunk: Buffer) => { raw += chunk.toString("utf8"); });
        req.on("end", async () => {
          try {
            const body = JSON.parse(raw) as { tracePath?: string; stagePath?: string };
            if (!body.tracePath || !path.isAbsolute(body.tracePath) || !body.stagePath) {
              throw new Error("Нужны абсолютный tracePath и относительный stagePath");
            }
            const traceDir = path.dirname(path.resolve(body.tracePath));
            const target = path.resolve(traceDir, body.stagePath);
            const relative = path.relative(traceDir, target);
            if (relative.startsWith("..") || path.isAbsolute(relative)) {
              throw new Error("Stage path выходит за пределы директории trace");
            }
            const info = await stat(target);
            if (!info.isFile()) throw new Error("Stage path не является файлом");
            const text = await readFile(target, "utf8");
            res.statusCode = 200;
            res.setHeader("Content-Type", "application/json; charset=utf-8");
            res.end(JSON.stringify({ text }));
          } catch (error) {
            res.statusCode = 400;
            res.setHeader("Content-Type", "application/json; charset=utf-8");
            res.end(JSON.stringify({ error: error instanceof Error ? error.message : "Ошибка чтения файла" }));
          }
        });
      });

      server.middlewares.use("/__stepper/fs", async (req, res) => {
        if (req.method !== "GET") {
          res.statusCode = 405;
          res.end();
          return;
        }

        const respond = (status: number, payload: unknown) => {
          res.statusCode = status;
          res.setHeader("Content-Type", "application/json; charset=utf-8");
          res.end(JSON.stringify(payload));
        };

        try {
          const url = new URL(req.url ?? "", "http://localhost");
          const rawPath = (url.searchParams.get("path") ?? "").trim();
          const traceOnly = url.searchParams.get("files") === "trace";

          let dir = rawPath || homedir();
          if (!path.isAbsolute(dir)) throw new Error("Нужен абсолютный путь");

          const info = await stat(dir);
          if (!info.isDirectory()) throw new Error("Это не директория");

          const entries = await readdir(dir, { withFileTypes: true });
          const result: Array<{ name: string; path: string; kind: "dir" | "file" }> = [];

          for (const entry of entries) {
            if (result.length >= MAX_DIR_ENTRIES) break;
            if (entry.name.startsWith(".")) continue;

            const fullPath = path.join(dir, entry.name);

            if (entry.isDirectory()) {
              result.push({ name: entry.name, path: fullPath, kind: "dir" });
            } else if (entry.isFile() && entry.name.endsWith(".json")) {
              if (traceOnly && !(await looksLikeTrace(fullPath))) continue;
              result.push({ name: entry.name, path: fullPath, kind: "file" });
            }
          }

          result.sort((a, b) => {
            if (a.kind !== b.kind) return a.kind === "dir" ? -1 : 1;
            return a.name.localeCompare(b.name);
          });

          const parent = path.dirname(dir);
          respond(200, {
            path: dir,
            parent: parent === dir ? null : parent,
            entries: result,
          });
        } catch (error) {
          respond(400, {
            error: error instanceof Error ? error.message : String(error),
          });
        }
      });
    },
  };
}
