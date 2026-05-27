import { useLayoutEffect, useRef, useEffect, useState } from 'react'
import { motion, AnimatePresence } from 'framer-motion'
import type { PlayersAround } from '../types/messages'
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
  const firstShown  = playersAround.length > 0 ? playersAround[0].position  : position
  const lastShown   = playersAround.length > 0 ? playersAround[playersAround.length - 1].position : position
  const aheadCount  = firstShown - 1
  const behindCount = queueSize - lastShown

  // Keyed by username for stable FLIP identity
  const entryRefs  = useRef<Map<string, HTMLDivElement>>(new Map())
  // Y positions from the previous render (populated at end of each layout effect)
  const prevRects  = useRef<Map<string, number>>(new Map())

  // Detect self-entry advancing (position decreased) for highlight flash
  const prevSelfPos = useRef<number | null>(null)
  const [advancing, setAdvancing] = useState(false)

  useEffect(() => {
    const self = playersAround.find(e => e.is_self)
    if (!self) return
    if (prevSelfPos.current !== null && self.position < prevSelfPos.current) {
      setAdvancing(true)
      const t = setTimeout(() => setAdvancing(false), 700)
      prevSelfPos.current = self.position
      return () => clearTimeout(t)
    }
    prevSelfPos.current = self.position
  }, [playersAround])

  useLayoutEffect(() => {
    // Capture current ("Last") positions of all mounted entries
    const newRects = new Map<string, number>()
    entryRefs.current.forEach((el, username) => {
      newRects.set(username, el.getBoundingClientRect().top)
    })

    // FLIP: for entries that existed before, animate from their old position
    entryRefs.current.forEach((el, username) => {
      const prev = prevRects.current.get(username)
      if (prev === undefined) return  // new entry — CSS handles enter animation
      const curr = newRects.get(username)
      if (curr === undefined) return
      const deltaY = prev - curr
      if (Math.abs(deltaY) < 1) return

      el.style.transform = `translateY(${deltaY}px)`
      el.style.transition = 'none'
      // Double rAF: first lets the browser paint the initial transform,
      // second triggers the transition back to identity.
      requestAnimationFrame(() => {
        requestAnimationFrame(() => {
          el.style.transform = ''
          el.style.transition = 'transform 0.38s cubic-bezier(0.25, 0.46, 0.45, 0.94)'
        })
      })
    })

    prevRects.current = newRects
  }, [playersAround])

  return (
    <div className="qp" role="dialog" aria-label="Queue position">
      <motion.div
        className="qp__card"
        initial={{ opacity: 0, y: 24 }}
        animate={{ opacity: 1, y: 0 }}
        exit={{ opacity: 0, y: 16 }}
        transition={{ duration: 0.28, ease: [0.16, 1, 0.3, 1] }}
      >
        <h3 className="qp__title">
          <span className="qp__pulse" aria-hidden="true" />
          Waiting to join
        </h3>
        <p className="qp__summary">
          You are{' '}
          <AnimatePresence mode="wait">
            <motion.strong
              key={position}
              className="qp__position-num"
              initial={{ opacity: 0, y: -4 }}
              animate={{ opacity: 1, y: 0 }}
              exit={{ opacity: 0, y: 4 }}
              transition={{ duration: 0.15 }}
            >
              #{position}
            </motion.strong>
          </AnimatePresence>
          {' '}of {queueSize} waiting
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

          {playersAround.map(entry => {
            const isNew = !prevRects.current.has(entry.username)
            return (
              <div
                key={entry.position}
                ref={el => {
                  if (el) entryRefs.current.set(entry.username, el)
                  else    entryRefs.current.delete(entry.username)
                }}
                className={[
                  'qp__entry',
                  entry.is_self ? 'qp__entry--self' : '',
                  entry.is_self && advancing ? 'qp__entry--advancing' : '',
                ].filter(Boolean).join(' ')}
                aria-current={entry.is_self ? 'true' : undefined}
                data-new={isNew ? 'true' : undefined}
              >
                <span className="qp__entry-pos">#{entry.position}</span>
                <span className="qp__entry-name">
                  {entry.is_self ? 'You' : entry.username}
                </span>
                {entry.is_self && advancing && (
                  <span className="qp__entry-badge" aria-hidden="true">↑</span>
                )}
              </div>
            )
          })}

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
      </motion.div>
    </div>
  )
}
