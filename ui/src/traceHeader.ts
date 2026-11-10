const PREFIX_BYTES = 64 * 1024;

export function pickHeader(text: string): Record<string, unknown> | null {
    const str = (key: string) =>
        new RegExp(`"${key}"\\s*:\\s*"([^"]*)"`).exec(text)?.[1];

    const num = (key: string) => {
        const m = new RegExp(
            `"${key}"\\s*:\\s*(\\d+)\\s*[,}]`,
        ).exec(text);

        return m ? Number(m[1]) : undefined;
    };

    const bool = (key: string) => {
        const m = new RegExp(
            `"${key}"\\s*:\\s*(true|false)\\b`,
        ).exec(text);

        return m ? m[1] === "true" : undefined;
    };

    const header = {
        schema: str("schema"),
        variant: str("variant"),
        total_events: num("total_events"),
        recorded_events: num("recorded_events"),
        truncated: bool("truncated"),
        valid: bool("valid"),
        complete: bool("complete"),
    };

    return Object.values(header).every(
        (value) => value !== undefined,
    )
        ? header
        : null;
}

export async function readTraceHeader(file: File): Promise<unknown> {
    const text = await file.slice(0, PREFIX_BYTES).text();
    const header = pickHeader(text);

    if (!header) {
        throw new Error("Не удалось прочитать заголовок trace.json");
    }

    return header;
}
