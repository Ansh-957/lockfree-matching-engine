import { useEffect, useRef, useState } from 'react'
import { useEngineSocket } from './lib/useEngineSocket'
import HeaderBar from './components/HeaderBar'
import OrderBookLadder from './components/OrderBookLadder'
import TradeTape from './components/TradeTape'
import LatencyChart from './components/LatencyChart'
import StatusBar from './components/StatsStrip'
import { fmtNs } from './lib/format'

const HISTORY_KEEP = 240
const TAPE_KEEP = 80

const EMPTY_STATS = {
  match_p50: 0, match_p95: 0, match_p99: 0,
  order_p50: 0, order_p99: 0,
  processed: 0, fills: 0,
  pool_free: 0, pool_capacity: 0,
  dropped: 0, clients: 0, book_bids: 0, book_asks: 0,
}

export default function App() {
  const { snapshot, status, url } = useEngineSocket()

  const prevRef = useRef(null)
  const [rates, setRates] = useState({ msgRate: 0, fillRate: 0 })
  const [history, setHistory] = useState([])

  const lastSeqRef = useRef(0)
  const lastPxRef = useRef(null)
  const [tape, setTape] = useState([])
  const [last, setLast] = useState(null)
  const [lastDir, setLastDir] = useState(0)

  useEffect(() => {
    if (!snapshot) return
    const s = snapshot.stats

    const prev = prevRef.current
    prevRef.current = { t: snapshot.t, processed: s.processed, fills: s.fills }
    if (prev && snapshot.t > prev.t) {
      const dt = (snapshot.t - prev.t) / 1000
      const inst = {
        msgRate: Math.max(0, (s.processed - prev.processed) / dt),
        fillRate: Math.max(0, (s.fills - prev.fills) / dt),
      }
      const a = 0.25
      setRates((r) => ({
        msgRate: r.msgRate ? r.msgRate * (1 - a) + inst.msgRate * a : inst.msgRate,
        fillRate: r.fillRate ? r.fillRate * (1 - a) + inst.fillRate * a : inst.fillRate,
      }))
    }

    if (s.match_p50 > 0) {
      setHistory((h) =>
        [...h, { t: snapshot.t, p50: s.match_p50, p95: s.match_p95, p99: s.match_p99 }]
          .slice(-HISTORY_KEEP),
      )
    }

    if (snapshot.fills?.length) {
      const fresh = snapshot.fills
        .filter(([seq]) => seq > lastSeqRef.current)
        .map(([seq, ts, price, qty, buy]) => ({ seq, ts, price, qty, buy: buy === 1 }))
      if (fresh.length) {
        lastSeqRef.current = fresh[fresh.length - 1].seq
        const px = fresh[fresh.length - 1].price
        if (lastPxRef.current != null) {
          setLastDir(Math.sign(px - lastPxRef.current))
        }
        lastPxRef.current = px
        setLast(px)
        setTape((t) => [...fresh.slice().reverse(), ...t].slice(0, TAPE_KEEP))
      }
    }
  }, [snapshot])

  const stats = snapshot?.stats ?? EMPTY_STATS
  const bids = snapshot?.bids ?? []
  const asks = snapshot?.asks ?? []
  const stale = status !== 'live' && snapshot != null

  const orderNote = stats.order_p50
    ? `e2e ${fmtNs(stats.order_p50)} / ${fmtNs(stats.order_p99)}`
    : 'rdtsc · log y'

  return (
    <div className={`app ${stale ? 'stale' : ''}`}>
      <HeaderBar
        snapshot={snapshot}
        status={status}
        rates={rates}
        last={last}
        lastDir={lastDir}
      />

      {status === 'down' && (
        <div className="banner">NO ENGINE at {url} — retrying</div>
      )}

      <div className="workspace">
        <div className="pane">
          <div className="pane-head">
            <span className="pane-name">Book</span>
            <span className="pane-meta num">
              {Math.max(bids.length, asks.length) || 0} lv
            </span>
          </div>
          <div className="pane-body">
            <OrderBookLadder bids={bids} asks={asks} last={last} />
          </div>
        </div>

        <div className="pane">
          <div className="pane-head">
            <span className="pane-name">Match latency</span>
            <span className="pane-meta num">{orderNote}</span>
          </div>
          <div className="pane-body">
            <LatencyChart history={history} />
          </div>
        </div>

        <div className="pane">
          <div className="pane-head">
            <span className="pane-name">Tape</span>
            <span className="pane-meta num">
              {stats.fills ? `${stats.fills.toLocaleString()} tot` : ''}
            </span>
          </div>
          <div className="pane-body">
            <TradeTape fills={tape} />
          </div>
        </div>
      </div>

      <StatusBar stats={stats} rates={rates} url={url} />
    </div>
  )
}
