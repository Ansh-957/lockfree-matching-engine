import { fmtInt, fmtPrice, fmtTime } from '../lib/format'

export default function TradeTape({ fills }) {
  if (!fills.length) {
    return <div className="empty">NO PRINTS</div>
  }

  return (
    <div className="tape">
      <div className="tape-cols">
        <span></span>
        <span>Price</span>
        <span>Size</span>
        <span>Time</span>
      </div>
      {fills.map((f) => (
        <div className={`tape-row ${f.buy ? 'buy' : 'sell'}`} key={f.seq}>
          <span className="side">{f.buy ? 'B' : 'S'}</span>
          <span className="px num">{fmtPrice(f.price)}</span>
          <span className="sz num">{fmtInt(f.qty)}</span>
          <span className="time num">{fmtTime(f.ts)}</span>
        </div>
      ))}
    </div>
  )
}
