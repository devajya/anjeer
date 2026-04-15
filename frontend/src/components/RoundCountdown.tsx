import { useEffect, useRef, useState } from 'react'
import './RoundCountdown.css'

interface RoundCountdownProps {
  /** ISO 8601 UTC timestamp for pre-deal countdown. null = no pre-deal countdown. */
  startsAt: string | null
  /**
   * ISO 8601 UTC timestamp for when the active round expires.
   * null = no active round (waiting or round ended).
   */
  roundEndAt: string | null
}

// AGENT-CTX: formats seconds as MM:SS for the active-round timer display.
function formatMMSS(totalSeconds: number): string {
  const mins = Math.floor(totalSeconds / 60)
  const secs = totalSeconds % 60
  return `${String(mins).padStart(2, '0')}:${String(secs).padStart(2, '0')}`
}

export function RoundCountdown({ startsAt, roundEndAt }: RoundCountdownProps) {
  const [secondsLeft, setSecondsLeft] = useState<number | null>(null)
  const intervalRef = useRef<ReturnType<typeof setInterval> | null>(null)

  // AGENT-CTX: A single effect keyed to the active timestamp avoids a flash of
  // null between startsAt clearing and roundEndAt being set (they transition
  // atomically in the round_start handler). roundEndAt takes priority so
  // the active-round timer renders immediately when the deal fires.
  const activeTimestamp = roundEndAt ?? startsAt
  const isActiveRound   = roundEndAt !== null

  useEffect(() => {
    // Clear any previous interval before re-computing.
    if (intervalRef.current !== null) {
      clearInterval(intervalRef.current)
      intervalRef.current = null
    }

    if (!activeTimestamp) {
      setSecondsLeft(null)
      return
    }

    const targetMs = new Date(activeTimestamp).getTime()
    const tick = () => {
      const remaining = Math.max(0, Math.ceil((targetMs - Date.now()) / 1000))
      setSecondsLeft(remaining)
      // Stop ticking once we reach zero; display stays until the prop goes null.
      if (remaining <= 0 && intervalRef.current !== null) {
        clearInterval(intervalRef.current)
        intervalRef.current = null
      }
    }
    tick()
    intervalRef.current = setInterval(tick, 250)

    return () => {
      if (intervalRef.current !== null) {
        clearInterval(intervalRef.current)
        intervalRef.current = null
      }
    }
  }, [activeTimestamp])

  if (secondsLeft === null) return null

  if (isActiveRound) {
    // AGENT-CTX: Pulse animation on expiring is intentional alert behaviour —
    // it is the final-30s urgency signal, not a cosmetic UI transition.
    // Slice 8 forbids animations during active trading; this is a timer widget
    // in the header, not part of the trading interface.
    const expiring = secondsLeft <= 30
    return (
      <div
        className={`round-countdown round-countdown--active${expiring ? ' round-countdown--expiring' : ''}`}
        role="timer"
        aria-live="polite"
      >
        Round ends in{' '}
        <span className="round-countdown__time">{formatMMSS(secondsLeft)}</span>
      </div>
    )
  }

  // Pre-deal countdown: hide once elapsed (0 means deal is imminent, not "over").
  if (secondsLeft <= 0) return null
  return (
    <div className="round-countdown" role="status" aria-live="polite">
      Round starting in <span className="round-countdown__count">{secondsLeft}</span>…
    </div>
  )
}
