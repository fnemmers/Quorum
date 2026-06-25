import { sectorOf, GICS_SECTORS, type GicsSector, SECTOR_COLOR } from '../data/gicsSectors';

/*
 * Diversity score for a top-K consensus list.
 *
 * We use the sector Herfindahl-Hirschman index (HHI) — the standard
 * concentration measure in equity portfolio management.
 *
 *     w_s   = (count of picks in sector s) / total picks
 *     HHI   = sum_s (w_s)^2                             ∈ (0, 1]
 *     N_eff = 1 / HHI                                   "effective sectors"
 *     score = (N_eff - 1) / (S - 1)                     ∈ [0, 1]
 *
 * where S = 11 GICS sectors. A pure-tech basket scores 0, an equal
 * 11-sector split scores 1.
 *
 * "Other" (unmapped tickers) is treated as a 12th bucket so unmapped
 * names don't inflate diversity unrealistically — but it's flagged so
 * the user knows the score is conservative when many picks are
 * uncategorised.
 */

export interface SectorWeight {
  sector: GicsSector;
  count:  number;
  weight: number;            // 0..1
  color:  string;
}

export interface Diversity {
  total:               number;
  hhi:                 number;             // sum of squared weights
  effectiveSectors:    number;             // 1 / hhi
  score:               number;             // 0..1 normalized
  breakdown:           SectorWeight[];     // sorted desc by weight
  unmappedCount:       number;             // 'Other' bucket size
  topSectorWeight:     number;             // largest single sector weight
  topSector:           GicsSector | null;
}

export function computeDiversity(symbols: string[]): Diversity {
  const total = symbols.length;
  if (total === 0) {
    return {
      total: 0, hhi: 0, effectiveSectors: 0, score: 0,
      breakdown: [], unmappedCount: 0,
      topSectorWeight: 0, topSector: null,
    };
  }

  const counts = new Map<GicsSector, number>();
  let unmapped = 0;
  for (const sym of symbols) {
    const s = sectorOf(sym);
    if (s === 'Other') unmapped++;
    counts.set(s, (counts.get(s) ?? 0) + 1);
  }

  const breakdown: SectorWeight[] = [];
  let hhi = 0;
  for (const [sector, count] of counts) {
    const w = count / total;
    hhi += w * w;
    breakdown.push({ sector, count, weight: w, color: SECTOR_COLOR[sector] });
  }
  breakdown.sort((a, b) => b.weight - a.weight);

  const effectiveSectors = hhi > 0 ? 1 / hhi : 0;
  // Use 11 GICS sectors as the max-diversity reference. (Even if some
  // picks land in 'Other', N_eff can exceed 11 in pathological cases —
  // clip to keep score in [0,1].)
  const score = Math.max(0, Math.min(1, (effectiveSectors - 1) / (GICS_SECTORS.length - 1)));

  return {
    total,
    hhi,
    effectiveSectors,
    score,
    breakdown,
    unmappedCount: unmapped,
    topSectorWeight: breakdown[0]?.weight ?? 0,
    topSector:       breakdown[0]?.sector ?? null,
  };
}
