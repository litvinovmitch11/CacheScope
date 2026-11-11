import { readFile, stat } from "node:fs/promises";
import path from "node:path";
import type { Plugin } from "vite";

/** Dev-only local file reader. It reads only stage files located under trace directory. */
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
    },
  };
}
