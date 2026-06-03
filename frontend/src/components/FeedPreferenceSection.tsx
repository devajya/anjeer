import { useState } from 'react'
import './FeedPreferenceSection.css'

type FeedTier = 'mbp1' | 'mbpn' | 'mbo'

const FEED_CARDS: { tier: FeedTier; label: string; description: string }[] = [
  {
    tier: 'mbp1',
    label: 'MBP-1',
    description: 'Best bid and ask only. Lowest bandwidth — ideal for scripts and fast execution.',
  },
  {
    tier: 'mbpn',
    label: 'MBP-N',
    description: 'Full price ladder with aggregated quantity at each level. Shows market depth.',
  },
  {
    tier: 'mbo',
    label: 'MBO',
    description: 'Every individual order event — adds, executes, cancels. Highest fidelity.',
  },
]

export function FeedPreferenceSection() {
  const [selected, setSelected] = useState<FeedTier | null>(null)
  const [saving, setSaving] = useState(false)

  async function handleSelect(tier: FeedTier) {
    if (tier === selected || saving) return
    setSelected(tier)
    setSaving(true)
    try {
      await fetch('/players/me/feed', {
        method: 'PUT',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ feed_preference: tier }),
      })
    } finally {
      setSaving(false)
    }
  }

  return (
    <div className="feed-pref">
      <h2 className="feed-pref__heading">Market Data Feed</h2>
      <p className="feed-pref__sub">
        Preference is applied at WS upgrade time — reconnect after changing.
      </p>
      <div className="feed-pref__cards">
        {FEED_CARDS.map(({ tier, label, description }) => (
          <button
            key={tier}
            className={[
              'feed-pref__card',
              selected === tier ? 'feed-pref__card--selected' : '',
            ].join(' ').trim()}
            onClick={() => handleSelect(tier)}
            aria-pressed={selected === tier}
            aria-label={`Select ${label} feed`}
          >
            <div className="feed-pref__card-media">
              <video
                autoPlay
                loop
                muted
                playsInline
                className="feed-pref__card-video"
              />
            </div>
            <div className="feed-pref__card-body">
              <span className="feed-pref__card-label">{label}</span>
              <span className="feed-pref__card-desc">{description}</span>
            </div>
          </button>
        ))}
      </div>
    </div>
  )
}
