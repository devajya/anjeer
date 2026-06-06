import './MbpNDepthPanel.css'
import { SUIT_SYMBOLS, SUIT_ORDER, suitClass } from '../utils/suits'

interface DepthLevel { price: number; qty: number }
interface SuitDepth  { bids: DepthLevel[]; asks: DepthLevel[] }

interface Props {
  bookDepths: Record<string, SuitDepth>
}

function SuitDepthSection({ suit, depth }: { suit: string; depth: SuitDepth | undefined }) {
  const bids = depth?.bids.slice(0, 5) ?? []
  const asks = depth?.asks.slice(0, 5) ?? []

  return (
    <div className="mbpn__section">
      <div className="mbpn__section-hdr">
        <span className={`mbpn__suit-symbol ${suitClass(suit)}`}>{SUIT_SYMBOLS[suit] ?? suit}</span>
        <span className="mbpn__suit-name">{suit}</span>
      </div>
      <div className="mbpn__ladder">
        <div className="mbpn__col mbpn__col--bids">
          <div className="mbpn__col-hdr">Bids</div>
          {bids.length === 0 ? (
            <div className="mbpn__empty">—</div>
          ) : (
            bids.map((lvl, i) => (
              <div key={i} className="mbpn__row mbpn__row--bid">
                <span className="mbpn__qty">{lvl.qty}</span>
                <span className="mbpn__at">@</span>
                <span className="mbpn__price">{lvl.price}</span>
              </div>
            ))
          )}
        </div>
        <div className="mbpn__col mbpn__col--asks">
          <div className="mbpn__col-hdr">Asks</div>
          {asks.length === 0 ? (
            <div className="mbpn__empty">—</div>
          ) : (
            asks.map((lvl, i) => (
              <div key={i} className="mbpn__row mbpn__row--ask">
                <span className="mbpn__qty">{lvl.qty}</span>
                <span className="mbpn__at">@</span>
                <span className="mbpn__price">{lvl.price}</span>
              </div>
            ))
          )}
        </div>
      </div>
    </div>
  )
}

export function MbpNDepthPanel({ bookDepths }: Props) {
  return (
    <div className="mbpn">
      {SUIT_ORDER.map(suit => (
        <SuitDepthSection key={suit} suit={suit} depth={bookDepths[suit]} />
      ))}
    </div>
  )
}
