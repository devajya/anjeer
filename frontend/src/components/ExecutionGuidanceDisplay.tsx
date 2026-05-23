import type { EvalExecutionGuidanceMessage, EvalSuitGuidance } from '../types/messages'
import './ExecutionGuidanceDisplay.css'

const SUIT_SYMBOLS: Record<string, string> = {
  clubs: '♣', diamonds: '♦', hearts: '♥', spades: '♠',
}
const SUIT_COLORS: Record<string, string> = {
  clubs: '#2a7a2a', diamonds: '#1a5fc8', hearts: '#c0392b', spades: '#b0b0c8',
}
const SUIT_ORDER = ['clubs', 'diamonds', 'hearts', 'spades']

const ACTION_CLASS: Record<string, string> = {
  buy:  'execution-guidance__badge--buy',
  sell: 'execution-guidance__badge--sell',
  hold: 'execution-guidance__badge--hold',
}

const REC_LABEL: Record<string, string> = {
  passive:    'passive',
  aggressive: 'aggr.',
  hold:       'hold',
}

function SuitRow({ name, g }: { name: string; g: EvalSuitGuidance }) {
  const color  = SUIT_COLORS[name] ?? '#aaa'
  const symbol = SUIT_SYMBOLS[name] ?? name
  const fp     = (g.fill_probability * 100).toFixed(0) + '%'
  const pev    = g.passive_ev >= 0
    ? '+' + g.passive_ev.toFixed(2)
    : g.passive_ev.toFixed(2)
  const pevClass = g.passive_ev > 0 ? 'positive' : g.passive_ev < 0 ? 'negative' : ''

  return (
    <tr className="execution-suit-row">
      <td className="execution-suit-row__suit" style={{ color }}>
        {symbol}
      </td>
      <td className="execution-suit-row__fp">{fp}</td>
      <td className={`execution-suit-row__pev ${pevClass}`}>{pev}</td>
      <td className="execution-suit-row__intensity">{g.trade_intensity[0].toUpperCase()}</td>
      <td className={`execution-suit-row__rec execution-suit-row__rec--${g.recommendation}`}>
        {REC_LABEL[g.recommendation] ?? g.recommendation}
      </td>
    </tr>
  )
}

interface Props {
  executionGuidance: EvalExecutionGuidanceMessage | null
}

export function ExecutionGuidanceDisplay({ executionGuidance }: Props) {
  if (!executionGuidance) {
    return (
      <div className="execution-guidance execution-guidance--empty" data-testid="execution-guidance">
        <span className="execution-guidance__placeholder">Awaiting execution guidance…</span>
      </div>
    )
  }

  const { action, suit, price, suits } = executionGuidance
  const hasSuits = suits && Object.keys(suits).length > 0

  return (
    <div className="execution-guidance" data-testid="execution-guidance">
      <h4 className="execution-guidance__heading">Execution Guidance</h4>

      {/* Top recommendation */}
      <div className="execution-guidance__row" data-testid="execution-guidance-row">
        <span
          className={`execution-guidance__badge ${ACTION_CLASS[action] ?? ''}`}
          data-testid="execution-guidance-action"
        >
          {action.toUpperCase()}
        </span>
        {suit && (
          <span
            className="execution-guidance__suit"
            style={{ color: SUIT_COLORS[suit] ?? '#aaa' }}
            data-testid="execution-guidance-suit"
          >
            {SUIT_SYMBOLS[suit] ?? suit}
          </span>
        )}
        {price !== null && action !== 'hold' && (
          <span className="execution-guidance__price" data-testid="execution-guidance-price">
            @ {price}
          </span>
        )}
      </div>

      {/* Per-suit breakdown table */}
      {hasSuits && (
        <table className="execution-suit-table" data-testid="execution-suit-table">
          <thead>
            <tr>
              <th title="Suit">S</th>
              <th title="Fill probability">FP</th>
              <th title="Passive EV">pEV</th>
              <th title="Trade intensity (L/M/H)">I</th>
              <th title="Recommendation">Rec</th>
            </tr>
          </thead>
          <tbody>
            {SUIT_ORDER.filter(s => suits[s]).map(s => (
              <SuitRow key={s} name={s} g={suits[s]} />
            ))}
          </tbody>
        </table>
      )}
    </div>
  )
}
