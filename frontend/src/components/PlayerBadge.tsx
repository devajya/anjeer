import './PlayerBadge.css'

interface Props {
  username:  string
  balance:   number | null
  onLogout:  () => Promise<void>
}

export function PlayerBadge({ username, balance, onLogout }: Props) {
  return (
    <div className="player-badge">
      <span className="player-badge__username">{username}</span>
      {balance !== null && (
        // AGENT-CTX: toLocaleString() for thousands separators — purely cosmetic.
        // The balance unit is integer points matching the engine's int64_t.
        <span className="player-badge__balance">{balance.toLocaleString()}</span>
      )}
      <button className="player-badge__logout" onClick={onLogout}>
        Log out
      </button>
    </div>
  )
}
