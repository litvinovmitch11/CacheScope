import { defaultHierarchy } from "./experimentApi";
import type { CacheLevel, HierarchyConfig } from "./experimentApi";

export interface CachePreset {
  id: string; name: string; description: string;
  sources: readonly { title: string; url: string }[];
  levels: ReadonlyArray<Readonly<CacheLevel>>;
  hierarchy: HierarchyConfig;
}
// Admit a CPU only when every exposed setting matches cited hardware docs.
// The parameter/source matrix and scope are in docs/PROCESSOR_PRESET_AUDIT.md.
const manual = "https://bitsavers.trailing-edge.com/components/intel/80486/273021-001_Embedded_Intel486_Processor_Family_Developers_Manual_199710.pdf";
const scope = "Встроенный объединённый L1 для системы без внешнего L2: межуровневой связи нет. Трасса содержит обращения к данным, без выборки команд, snoop и задержек шин.";
export const CACHE_PRESETS: readonly CachePreset[] = [
  {
    id: "intel-dx2-l1-wt", name: "IntelDX2 — встроенный L1, WT",
    description: "8 KiB, 16 B, 4-way, Intel tree-PLRU, write-through, no-write-allocate. Обычная кэшируемая память, кэш включён. " + scope,
    sources: [
      { title: "Размер / строка / ways: Intel §7.1, рис. 7-1", url: manual + "#page=179" },
      { title: "Запись / промах записи: Intel §7.1", url: manual + "#page=180" },
      { title: "Замещение PLRU: Intel §3.2.3 и §7.5, рис. 7-2", url: manual + "#page=186" },
      { title: "Система без внешнего L2: Intel рис. 3-9", url: manual + "#page=65" },
    ],
    levels: [{ name: "L1", size_bytes: 8192, line_size_bytes: 16, associativity: 4,
      replacement_policy: "PLRU4", write_policy: "write_through", allocation_policy: "no_write_allocate" }],
    hierarchy: defaultHierarchy(1),
  },
  {
    id: "intel-dx4-wb-l1", name: "Write-Back Enhanced IntelDX4 — встроенный L1, WB",
    description: "16 KiB, 16 B, 4-way, Intel tree-PLRU, write-back, no-write-allocate. Выбран Enhanced Bus Mode: WB/WT#=1, PWT=0, кэшируемые private-линии E/M. " + scope,
    sources: [
      { title: "Размер / строка / ways / WB-режим: Intel §7.1.1–7.1.2", url: manual + "#page=181" },
      { title: "Запись WB и промах NWA: Intel табл. 7-5 и 7-6", url: manual + "#page=194" },
      { title: "Замещение PLRU: Intel §3.2.3 и §7.5, рис. 7-2", url: manual + "#page=186" },
      { title: "Система без внешнего L2: Intel рис. 3-9", url: manual + "#page=65" },
    ],
    levels: [{ name: "L1", size_bytes: 16384, line_size_bytes: 16, associativity: 4,
      replacement_policy: "PLRU4", write_policy: "write_back", allocation_policy: "no_write_allocate" }],
    hierarchy: defaultHierarchy(1),
  },
];

export function findCachePreset(levels: readonly CacheLevel[], hierarchy = defaultHierarchy(levels.length), preferred?: string): CachePreset | undefined {
  const fields: (keyof CacheLevel)[] = ["name", "size_bytes", "line_size_bytes", "associativity", "replacement_policy", "random_seed"];
  const matches = (preset: CachePreset) => preset.levels.length === levels.length &&
    preset.levels.every((level, i) => fields.every(field => level[field] === levels[i][field]) &&
      (level.write_policy ?? "write_back") === (levels[i].write_policy ?? "write_back") &&
      (level.allocation_policy ?? "write_allocate") === (levels[i].allocation_policy ?? "write_allocate")) &&
    JSON.stringify(preset.hierarchy.inclusion_policies) === JSON.stringify(hierarchy.inclusion_policies) &&
    preset.hierarchy.writeback_clean === hierarchy.writeback_clean &&
    preset.hierarchy.non_inclusive_fill === hierarchy.non_inclusive_fill;
  return CACHE_PRESETS.find(preset => preset.id === preferred && matches(preset)) ?? CACHE_PRESETS.find(matches);
}
