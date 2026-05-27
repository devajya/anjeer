import './PlayerBadge.css'

interface Props {
  username: string
  onLeave:  () => void
}

export function PlayerBadge({ username, onLeave }: Props) {
  return (
    <div className="player-badge">
      <span className="player-badge__username">{username}</span>
      <button className="player-badge__leave" onClick={onLeave}>
        Leave game
      </button>
    </div>
  )
}
