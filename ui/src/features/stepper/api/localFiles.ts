export async function readStageText(tracePath: string, stagePath: string): Promise<string> {
  const response = await fetch("/__stepper/read-stage", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({ tracePath, stagePath }),
  });
  const body = (await response.json()) as { text?: string; error?: string };
  if (!response.ok) throw new Error(body.error ?? `Не удалось прочитать stage-файл (${response.status})`);
  return body.text ?? "";
}
