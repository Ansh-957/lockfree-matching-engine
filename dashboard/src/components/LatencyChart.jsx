import { useEffect, useRef, useState } from 'react'
import { fmtNs } from '../lib/format'

const SERIES = [
  { key: 'p50', color: '#cfcfcf' },
  { key: 'p95', color: '#6a6a6a' },
  { key: 'p99', color: 'var(--amber)' },
]

const PAD_L = 36
const PAD_R = 8
const PAD_Y = 12

function useSize() {
  const ref = useRef(null)
  const [size, setSize] = useState({ w: 0, h: 0 })
  useEffect(() => {
    const el = ref.current
    if (!el) return
    const ro = new ResizeObserver(([e]) =>
      setSize({ w: e.contentRect.width, h: e.contentRect.height }),
    )
    ro.observe(el)
    return () => ro.disconnect()
  }, [])
  return [ref, size]
}

export default function LatencyChart({ history }) {
  const [ref, { w, h }] = useSize()
  const last = history.length ? history[history.length - 1] : null
  const ready = history.length >= 2 && w > 0 && h > 0

  let svg = null
  if (ready) {
    const values = history.flatMap((d) => [d.p50, d.p99]).filter((v) => v > 0)
    const lo = Math.pow(10, Math.floor(Math.log10(Math.max(10, Math.min(...values)))))
    const hi = Math.pow(10, Math.ceil(Math.log10(Math.max(lo * 10, Math.max(...values)))))

    const x = (i) => PAD_L + (i / (history.length - 1)) * (w - PAD_L - PAD_R)
    const y = (v) => {
      const clamped = Math.min(Math.max(v, lo), hi)
      const f = (Math.log10(clamped) - Math.log10(lo)) / (Math.log10(hi) - Math.log10(lo))
      return h - PAD_Y - f * (h - 2 * PAD_Y)
    }

    const grid = []
    for (let v = lo; v <= hi; v *= 10) grid.push(v)

    const linePoints = (key) =>
      history.map((d, i) => `${x(i).toFixed(1)},${y(d[key]).toFixed(1)}`).join(' ')

    const areaPath =
      `M ${x(0).toFixed(1)},${(h - PAD_Y).toFixed(1)} ` +
      history.map((d, i) => `L ${x(i).toFixed(1)},${y(d.p50).toFixed(1)}`).join(' ') +
      ` L ${x(history.length - 1).toFixed(1)},${(h - PAD_Y).toFixed(1)} Z`

    svg = (
      <svg className="chart-svg" width={w} height={h}>
        {grid.map((v) => (
          <g key={v}>
            <line className="chart-grid-line" x1={PAD_L} x2={w - PAD_R} y1={y(v)} y2={y(v)} />
            <text className="chart-grid-label" x={4} y={y(v) + 3}>
              {fmtNs(v)}
            </text>
          </g>
        ))}
        <path className="chart-area p50" d={areaPath} />
        {SERIES.map((s) => (
          <polyline key={s.key} className={`chart-line ${s.key}`} points={linePoints(s.key)} />
        ))}
      </svg>
    )
  }

  return (
    <div className="scope">
      <div className="readouts">
        <div className="ro p50">
          <div className="k">p50</div>
          <div className="v num">{fmtNs(last?.p50)}</div>
        </div>
        <div className="ro p95">
          <div className="k">p95</div>
          <div className="v num">{fmtNs(last?.p95)}</div>
        </div>
        <div className="ro p99">
          <div className="k">p99</div>
          <div className="v num">{fmtNs(last?.p99)}</div>
        </div>
      </div>
      <div className="chart" ref={ref}>
        {svg ?? <div className="empty">NO SAMPLES</div>}
      </div>
    </div>
  )
}
