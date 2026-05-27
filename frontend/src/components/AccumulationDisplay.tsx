import type { EvalAccumulationSignalMessage, EvalPlayerSignal } from '../types/messages'
import './AccumulationDisplay.css'

const SUIT_SYMBOLS: Record<string, string> = {
  clubs: '♣', diamonds: '♦', hearts: '♥', spades: '♠',
}

const SIGNAL_BADGE_CLASS: Record<EvalPlayerSignal['signal'], string> = {
  Normal:   'accumulation-display__badge--normal',
  Elevated: 'accumulation-display__badge--elevated',
  High:     'accumulation-display__badge--high',
}

interface Props {
  accumulationSignal: EvalAccumulationSignalMessage | null
}

export function AccumulationDisplay({ accumulationSignal }: Props) {
  if (!accumulationSignal?.players?.length) {
    return (
      <div className="accumulation-display accumulation-display--empty" data-testid="accumulation-display">
        <span className="accumulation-display__placeholder">Awaiting accumulation signal…</span>
      </div>
    )
  }

  return (
    <div className="accumulation-display" data-testid="accumulation-display">
      <h4 className="accumulation-display__heading">Accumulation Signals</h4>
      <div className="accumulation-display__players">
        {accumulationSignal.players.map(p => (
          <div
            key={p.slot}
            className="accumulation-display__card"
            data-testid={`accumulation-card-${p.slot}`}
          >
            <div className="accumulation-display__card-header">
              <span className="accumulation-display__player-name">{p.player}</span>
              <span
                className={`accumulation-display__badge ${SIGNAL_BADGE_CLASS[p.signal]}`}
                data-testid={`accumulation-badge-${p.slot}`}
              >
                {p.signal}
              </span>
            </div>
            <div className="accumulation-display__card-row">
              <span
                className={`accumulation-display__suit-chip accumulation-display__suit--${p.primary_suit}`}
                title={p.primary_suit}
                data-testid={`accumulation-suit-${p.slot}`}
              >
                {SUIT_SYMBOLS[p.primary_suit] ?? p.primary_suit}
              </span>
              <div className="accumulation-display__bar-track" title={`confidence ${(p.confidence * 100).toFixed(0)}%`}>
                <div
                  className="accumulation-display__bar-fill"
                  style={{ width: `${(p.confidence * 100).toFixed(1)}%` }}
                  data-testid={`accumulation-bar-${p.slot}`}
                />
              </div>
              <span className="accumulation-display__conf-label" data-testid={`accumulation-conf-${p.slot}`}>
                {(p.confidence * 100).toFixed(0)}%
              </span>
            </div>
          </div>
        ))}
      </div>
    </div>
  )
}
