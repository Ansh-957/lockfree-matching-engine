import { useRef } from 'react'
import { fmtInt, fmtPrice } from '../lib/format'

function Rows({ levels, side, maxCum, flashRef }) {
  let cum = 0
  const rows = levels.map(([price, qty, orders]) => {
    cum += qty
    return { price, qty, orders, cum }
  })
  const ordered = side === 'ask' ? [...rows].reverse() : rows

  return ordered.map((r, i) => {
    const isTouch = side === 'ask' ? i === ordered.length - 1 : i === 0
    const prev = flashRef.current.get(r.price)
    const flash = prev !== undefined && prev !== r.qty
    flashRef.current.set(r.price, r.qty)

    return (
      <div
        className={`ladder-row ${side}${isTouch ? ' touch' : ''}`}
        key={r.price}
        title={`cum ${fmtInt(r.cum)}`}
      >
        {flash && <span className="row-flash" />}
        <span className="bar" style={{ width: `${Math.min(100, (r.cum / maxCum) * 100)}%` }} />
        <span className="price num">{fmtPrice(r.price)}</span>
        <span className="qty num">{fmtInt(r.qty)}</span>
        <span className="cum num">{fmtInt(r.cum)}</span>
        <span className="n num">{r.orders}</span>
      </div>
    )
  })
}

export default function OrderBookLadder({ bids, asks, last }) {
  const flashRef = useRef(new Map())

  if (!bids.length && !asks.length) {
    return <div className="empty">NO BOOK</div>
  }

  const cumOf = (side) => side.reduce((a, [, q]) => a + q, 0)
  const maxCum = Math.max(cumOf(bids), cumOf(asks), 1)

  const bestBid = bids.length ? bids[0][0] : null
  const bestAsk = asks.length ? asks[0][0] : null
  const hasSpread = bestBid != null && bestAsk != null
  const mid = hasSpread ? (bestBid + bestAsk) / 2 : last

  // drop flash keys that left the book so the map stays bounded
  const live = new Set([...bids, ...asks].map(([p]) => p))
  for (const k of flashRef.current.keys()) {
    if (!live.has(k)) flashRef.current.delete(k)
  }

  return (
    <div className="ladder">
      <div className="ladder-cols">
        <span>Price</span>
        <span>Size</span>
        <span>Cum</span>
        <span>#</span>
      </div>

      <div className="ladder-side asks">
        <Rows levels={asks} side="ask" maxCum={maxCum} flashRef={flashRef} />
      </div>

      <div className="spread">
        <span className="spread-mid num">{mid != null ? fmtPrice(mid) : '—'}</span>
        <span className="spread-meta num">
          {hasSpread ? `${fmtPrice(bestAsk - bestBid)} spread` : 'one-sided'}
        </span>
      </div>

      <div className="ladder-side bids">
        <Rows levels={bids} side="bid" maxCum={maxCum} flashRef={flashRef} />
      </div>
    </div>
  )
}
