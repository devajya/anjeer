import type { PlayersAround } from '../hooks/useQueueSocket'
import './QueuePopup.css'

interface Props {
  lobbyId:       string
  position:      number
  queueSize:     number
  playersAround: PlayersAround[]
  onLeave:       () => void
  onSpectate:    () => void
}

export function QueuePopup({ position, queueSize, playersAround, onLeave, onSpectate }: Props) {
  // Derive how many players are hidden above and below the visible window.
  const firstShown  = playersAround.length > 0 ? playersAround[0].position  : position
  const lastShown   = playersAround.length > 0 ? playersAround[playersAround.length - 1].position : position
  const aheadCount  = firstShown - 1
  const behindCount = queueSize - lastShown

  return (
    <div className="qp" role="dialog" aria-label="Queue position">
      <div className="qp__card">
        <h3 className="qp__title">Waiting to join</h3>
        <p className="qp__summary">
          You are <strong>#{position}</strong> of {queueSize} waiting
        </p>

        <div className="qp__track" aria-label="Queue track">
          {aheadCount > 0 && (
            <div
              className="qp__truncation qp__truncation--top"
              aria-label={`${aheadCount} players ahead`}
            >
              · · · {aheadCount} ahead
            </div>
          )}

          {playersAround.map(entry => (
            <div
              key={entry.position}
              className={`qp__entry${entry.is_self ? ' qp__entry--self' : ''}`}
              aria-current={entry.is_self ? 'true' : undefined}
            >
              <span className="qp__entry-pos">#{entry.position}</span>
              <span className="qp__entry-name">
                {entry.is_self ? 'You' : entry.username}
              </span>
            </div>
          ))}

          {behindCount > 0 && (
            <div
              className="qp__truncation qp__truncation--bottom"
              aria-label={`${behindCount} players behind`}
            >
              · · · {behindCount} behind
            </div>
          )}
        </div>

        <div className="qp__actions">
          <button className="qp__btn qp__btn--leave"    onClick={onLeave}>
            Leave Queue
          </button>
          <button className="qp__btn qp__btn--spectate" onClick={onSpectate}>
            Spectate
          </button>
        </div>
      </div>
    </div>
  )
}
