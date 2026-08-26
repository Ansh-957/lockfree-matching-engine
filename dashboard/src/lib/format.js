// every number in the UI goes through one of these so columns stay aligned

const TICK = 0.01 // dollars per tick, mirrors core/types.h

export function ticksToPrice(ticks) {
  return ticks * TICK
}

export function fmtPrice(ticks) {
  return (ticks * TICK).toLocaleString('en-US', {
    minimumFractionDigits: 2,
    maximumFractionDigits: 2,
  })
}

export function fmtInt(n) {
  return Math.round(n).toLocaleString('en-US')
}

export function fmtCompact(n) {
  if (n == null || Number.isNaN(n)) return '—'
  const abs = Math.abs(n)
  if (abs >= 1e9) return (n / 1e9).toFixed(2) + 'B'
  if (abs >= 1e6) return (n / 1e6).toFixed(2) + 'M'
  if (abs >= 1e3) return (n / 1e3).toFixed(1) + 'K'
  return String(Math.round(n))
}

export function fmtNs(ns) {
  if (ns == null || ns === 0) return '—'
  if (ns < 1e3) return `${Math.round(ns)} ns`
  if (ns < 1e6) return `${(ns / 1e3).toFixed(ns < 1e4 ? 2 : 1)} µs`
  if (ns < 1e9) return `${(ns / 1e6).toFixed(1)} ms`
  return `${(ns / 1e9).toFixed(2)} s`
}

export function fmtTime(ms) {
  const d = new Date(ms)
  const hh = String(d.getHours()).padStart(2, '0')
  const mm = String(d.getMinutes()).padStart(2, '0')
  const ss = String(d.getSeconds()).padStart(2, '0')
  const mmm = String(d.getMilliseconds()).padStart(3, '0')
  return `${hh}:${mm}:${ss}.${mmm}`
}

export function fmtClock(d) {
  const hh = String(d.getHours()).padStart(2, '0')
  const mm = String(d.getMinutes()).padStart(2, '0')
  const ss = String(d.getSeconds()).padStart(2, '0')
  return `${hh}:${mm}:${ss}`
}

// spread in basis points of mid. 1 bp = 0.01% of mid
export function fmtBps(spreadTicks, midTicks) {
  if (!midTicks) return '—'
  const bps = (spreadTicks / midTicks) * 10_000
  return `${bps.toFixed(bps < 10 ? 2 : 1)} bp`
}
