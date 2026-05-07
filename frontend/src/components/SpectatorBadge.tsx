interface Props {
  count: number
}

export function SpectatorBadge({ count }: Props) {
  return (
    <span className="spectator-badge" aria-label={`${count} spectator${count !== 1 ? 's' : ''}`}>
      <span aria-hidden="true">👁</span> {count}
    </span>
  )
}
