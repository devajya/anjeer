import { useEffect, useRef, useState } from 'react'
import './RoundCountdown.css'

interface RoundCountdownProps {
  /** ISO 8601 UTC timestamp. null = no countdown in progress. */
  startsAt: string | null
}

export function RoundCountdown({ startsAt }: RoundCountdownProps) {
  const [secondsLeft, setSecondsLeft] = useState<number | null>(null)
  const intervalRef = useRef<ReturnType<typeof setInterval> | null>(null)

  useEffect(() => {
    if (!startsAt) {
      setSecondsLeft(null)
      return
    }
    const fireMs = new Date(startsAt).getTime()
    const tick = () => {
      const remaining = Math.ceil((fireMs - Date.now()) / 1000)
      if (remaining <= 0) {
        setSecondsLeft(0)
        if (intervalRef.current !== null) clearInterval(intervalRef.current)
        return
      }
      setSecondsLeft(remaining)
    }
    tick()
    intervalRef.current = setInterval(tick, 250)
    return () => {
      if (intervalRef.current !== null) clearInterval(intervalRef.current)
    }
  }, [startsAt])

  if (secondsLeft === null || secondsLeft <= 0) return null

  return (
    <div className="round-countdown" role="status" aria-live="polite">
      Round starting in <span className="round-countdown__count">{secondsLeft}</span>…
    </div>
  )
}
