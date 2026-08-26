import { useEffect, useState } from 'react'
import { fmtClock, fmtCompact, fmtNs, fmtPrice, fmtBps } from '../lib/format'

function p99Class(ns) {
  if (!ns) return ''
  if (ns < 1_000) return 'good'
  if (ns < 10_000) return 'warn'
  return 'bad'
}

const FEED = {
  live: 'feed up',
  connecting: 'connecting',
  down: 'no feed',
}

function Clock() {
  const [now, setNow] = useState(() => new Date())
  useEffect(() => {
    const id = setInterval(() => setNow(new Date()), 250)
    return () => clearInterval(id)
  }, [])
  return <span className="clock num">{fmtClock(now)}</span>
}

export default function HeaderBar({ snapshot, status, rates, last, lastDir }) {
  const stats = snapshot?.stats
  const mode = snapshot?.mode
  const product = snapshot?.product ?? '—'

  const bids = snapshot?.bids ?? []
  const asks = snapshot?.asks ?? []
  const bestBid = bids.length ? bids[0][0] : null
  const bestAsk = asks.length ? asks[0][0] : null
  const hasSpread = bestBid != null && bestAsk != null
  const mid = hasSpread ? (bestBid + bestAsk) / 2 : last
  const spread = hasSpread ? bestAsk - bestBid : null

  useEffect(() => {
    document.title = `${product} · matching engine`
  }, [product])

  return (
    <>
      <div className="chrome">
        <span className="sys">ME</span>
        <span className="product num">{product}</span>
        <span className={`tag ${mode === 'live' ? 'on' : ''}`}>
          {mode === 'live' ? 'Coinbase L2' : mode === 'synthetic' ? 'Synthetic' : 'Idle'}
        </span>
        <div className="chrome-right">
          <Clock />
          <div className={`feed ${status}`}>
            <i />
            {FEED[status]}
          </div>
        </div>
      </div>

      <div className="quote">
        <div className="qcell last">
          <span className="qlabel">Last</span>
          <span className={`qvalue num ${lastDir === 1 ? 'up' : lastDir === -1 ? 'dn' : ''}`}>
            {last != null ? fmtPrice(last) : '—'}
          </span>
        </div>
        <div className="qcell">
          <span className="qlabel">Bid</span>
          <span className="qvalue num bid">{bestBid != null ? fmtPrice(bestBid) : '—'}</span>
        </div>
        <div className="qcell">
          <span className="qlabel">Ask</span>
          <span className="qvalue num ask">{bestAsk != null ? fmtPrice(bestAsk) : '—'}</span>
        </div>
        <div className="qcell">
          <span className="qlabel">Spread</span>
          <span className="qvalue num">{spread != null ? fmtPrice(spread) : '—'}</span>
          <span className="qmeta num">{spread != null ? fmtBps(spread, mid) : ''}</span>
        </div>

        <div className="qcell wide" />

        <div className="qcell engine">
          <span className="qlabel">Msg/s</span>
          <span className="qvalue num">{fmtCompact(rates.msgRate)}</span>
        </div>
        <div className="qcell engine">
          <span className="qlabel">Match p50</span>
          <span className="qvalue num">{fmtNs(stats?.match_p50)}</span>
        </div>
        <div className="qcell engine">
          <span className="qlabel">Match p99</span>
          <span className={`qvalue num p99 ${p99Class(stats?.match_p99)}`}>
            {fmtNs(stats?.match_p99)}
          </span>
        </div>
      </div>
    </>
  )
}
