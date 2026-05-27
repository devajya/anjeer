import { useRef, useState, useEffect, useCallback } from 'react'
import './AutoAdvanceProgress.css'

export interface BlockContent {
  tag: string
  headline: string
  body: string
}

export interface AutoAdvanceProgressProps {
  reducedMotion: boolean
}

const BLOCKS: BlockContent[] = [
  {
    tag: 'Step 1',
    headline: 'Join a lobby',
    body: `Create a private lobby or browse open games. Invite friends with a six-character code, set the buy-in, choose how many rounds you want to play, and configure whether empty seats should be filled by bots while you wait. Games support two to four players — human, bot, or a mix — and can start the moment the host is ready.`,
  },
  {
    tag: 'Step 2',
    headline: 'Receive your hand',
    body: `At the start of each round the deck is distributed secretly. Every player holds a private hand and a hidden goal suit — the suit you need to accumulate to win. You can see how many cards others hold in aggregate, but not what suits they are. The only way to figure out what opponents are chasing is to watch which prices they push, which offers they lift, and which sides they ignore.`,
  },
  {
    tag: 'Step 3',
    headline: 'Trade to accumulate',
    body: `Place bids and asks on any of the four suits at integer prices. Every matched trade wipes the entire book — so timing matters as much as price. You can bluff a suit you don't want to drive others away, front-run a player whose order flow gives them away, or quietly accumulate at the ask while everyone else fights over something else. The market is the only communication channel.`,
  },
  {
    tag: 'Step 4',
    headline: 'Score the round',
    body: `When the clock expires, positions are evaluated. The player holding the majority of the goal suit's cards captures the pot — the sum of all buy-ins for the round. Bonus cards pay a flat rate per unit regardless of majority. Ties on majority split the pot evenly. After scoring, standings update and the next round begins with a fresh deal. The player with the highest balance when all rounds conclude wins.`,
  },
]

// Autoscroll speed when idle: ~2.5px/frame gives a ~6s traversal of one block at 1080p
const AUTO_SCROLL_PX_PER_FRAME = 2.5
const IDLE_THRESHOLD_MS = 3500

interface Vec2 { x: number; y: number }

const SEAT_POSITIONS: Vec2[] = [
  { x: 60,  y: 40  },
  { x: 240, y: 40  },
  { x: 60,  y: 200 },
  { x: 240, y: 200 },
]
const POT_POSITION: Vec2 = { x: 150, y: 120 }
const CHIP_COLORS = ['#c8b86b', '#8fa3b1', '#d4846a', '#82b99a']

interface ChipAnim {
  startTime: number
  durationMs: number
  stagger: number
  srcPositions: Vec2[]
  dstPositions: Vec2[]
  onComplete?: () => void
}

interface ChipRenderState {
  positions: Vec2[]
  anim: ChipAnim | null
}

function lerp(a: number, b: number, t: number) { return a + (b - a) * t }
function easeInOut(t: number) { return t < 0.5 ? 2 * t * t : -1 + (4 - 2 * t) * t }
function arcLerp(src: Vec2, dst: Vec2, t: number, h: number): Vec2 {
  return { x: lerp(src.x, dst.x, t), y: lerp(src.y, dst.y, t) - Math.sin(Math.PI * t) * h }
}

function buildAnim(index: number, positions: Vec2[]): ChipAnim {
  const src = positions.map(p => ({ ...p }))
  switch (index) {
    case 0: return {
      startTime: performance.now(), durationMs: 800, stagger: 80,
      srcPositions: src, dstPositions: Array(4).fill(null).map(() => ({ ...POT_POSITION })),
    }
    case 1: return {
      startTime: performance.now(), durationMs: 700, stagger: 60,
      srcPositions: src,
      dstPositions: [SEAT_POSITIONS[3], SEAT_POSITIONS[2], SEAT_POSITIONS[1], SEAT_POSITIONS[0]].map(p => ({ ...p })),
    }
    case 2: return {
      startTime: performance.now(), durationMs: 900, stagger: 90,
      srcPositions: src,
      dstPositions: [
        { x: 150, y: 60 }, { x: 240, y: 120 }, { x: 60, y: 120 }, { x: 150, y: 180 },
      ],
    }
    case 3: return {
      startTime: performance.now(), durationMs: 800, stagger: 70,
      srcPositions: src,
      dstPositions: Array(4).fill(null).map(() => ({ ...SEAT_POSITIONS[0] })),
    }
    default: return {
      startTime: performance.now(), durationMs: 500, stagger: 50,
      srcPositions: src, dstPositions: src.map(p => ({ ...p })),
    }
  }
}

export function AutoAdvanceProgress({ reducedMotion }: AutoAdvanceProgressProps) {
  const [activeIndex, setActiveIndex] = useState(0)
  const [progressPct, setProgressPct] = useState(0)
  const sectionRef = useRef<HTMLElement>(null)
  const canvasRef = useRef<HTMLCanvasElement>(null)
  const chipStateRef = useRef<ChipRenderState>({ positions: SEAT_POSITIONS.map(p => ({ ...p })), anim: null })
  const rafRef = useRef<number>(0)

  const triggerAnim = useCallback((index: number) => {
    const state = chipStateRef.current
    const anim = buildAnim(index, state.positions)
    anim.onComplete = () => {
      chipStateRef.current.positions = SEAT_POSITIONS.map(p => ({ ...p }))
      chipStateRef.current.anim = null
    }
    state.anim = anim
  }, [])

  // Canvas rAF draw loop — exits early in jsdom (getContext returns null)
  useEffect(() => {
    if (reducedMotion) return
    const canvas = canvasRef.current
    if (!canvas) return
    const ctx = canvas.getContext('2d')
    if (!ctx) return

    const CHIP_R = 10

    function drawFrame() {
      const state = chipStateRef.current
      const now = performance.now()
      ctx!.clearRect(0, 0, canvas!.width, canvas!.height)
      const pos = state.positions.map(p => ({ ...p }))

      if (state.anim) {
        const { startTime, durationMs, stagger, srcPositions, dstPositions, onComplete } = state.anim
        let allDone = true
        for (let i = 0; i < 4; i++) {
          const elapsed = now - (startTime + i * stagger)
          if (elapsed < 0) { pos[i] = { ...srcPositions[i] }; allDone = false }
          else if (elapsed >= durationMs) { pos[i] = { ...dstPositions[i] } }
          else { pos[i] = arcLerp(srcPositions[i], dstPositions[i], easeInOut(elapsed / durationMs), 40); allDone = false }
        }
        if (allDone) { state.positions = dstPositions.map(p => ({ ...p })); state.anim = null; onComplete?.() }
      }

      ctx!.beginPath()
      ctx!.arc(POT_POSITION.x, POT_POSITION.y, 8, 0, Math.PI * 2)
      ctx!.fillStyle = 'rgba(200,184,107,0.22)'
      ctx!.fill()
      ctx!.strokeStyle = 'rgba(200,184,107,0.65)'
      ctx!.lineWidth = 1
      ctx!.stroke()

      for (let i = 0; i < 4; i++) {
        ctx!.beginPath()
        ctx!.arc(pos[i].x, pos[i].y, CHIP_R, 0, Math.PI * 2)
        ctx!.fillStyle = CHIP_COLORS[i]
        ctx!.fill()
        ctx!.strokeStyle = 'rgba(0,0,0,0.15)'
        ctx!.lineWidth = 1.5
        ctx!.stroke()
      }

      rafRef.current = requestAnimationFrame(drawFrame)
    }

    rafRef.current = requestAnimationFrame(drawFrame)
    return () => cancelAnimationFrame(rafRef.current)
  }, [reducedMotion])

  useEffect(() => {
    if (!reducedMotion) triggerAnim(activeIndex)
  }, [activeIndex, reducedMotion, triggerAnim])

  // Scroll-driven progress + autoscroll idle fallback
  useEffect(() => {
    if (reducedMotion) return
    const section = sectionRef.current
    if (!section) return

    // Returns 0–1 progress across the whole section. Uses getBoundingClientRect
    // so it's always based on current scroll position without needing scrollY.
    function getProgress(): number {
      const rect = section!.getBoundingClientRect()
      const scrollable = section!.offsetHeight - window.innerHeight
      if (scrollable <= 0) return 0
      // rect.top is negative once we've scrolled past the section top
      return Math.max(0, Math.min(-rect.top / scrollable, 1))
    }

    // True when the sticky inner is filling the viewport
    function isActive(): boolean {
      const rect = section!.getBoundingClientRect()
      return rect.top <= 0 && rect.bottom >= window.innerHeight
    }

    function applyProgress(p: number) {
      const blockFloat = p * BLOCKS.length
      const newIdx = Math.min(Math.floor(blockFloat), BLOCKS.length - 1)
      const newPct = (blockFloat - newIdx) * 100
      setActiveIndex(newIdx)
      setProgressPct(Math.min(newPct, 100))
    }

    // Autoscroll rAF — scrollBy drives the scroll position, but we guard with
    // isAutoScrolling so the resulting scroll event doesn't cancel itself.
    let autoRaf: number | null = null
    let isAutoScrolling = false

    function startAutoScroll() {
      if (autoRaf !== null) return
      isAutoScrolling = true
      function step() {
        if (!isActive() || getProgress() >= 1) { autoRaf = null; isAutoScrolling = false; return }
        window.scrollBy(0, AUTO_SCROLL_PX_PER_FRAME)
        autoRaf = requestAnimationFrame(step)
      }
      autoRaf = requestAnimationFrame(step)
    }

    function stopAutoScroll() {
      if (autoRaf !== null) { cancelAnimationFrame(autoRaf); autoRaf = null }
      isAutoScrolling = false
    }

    let idleTimeout: ReturnType<typeof setTimeout> | null = null

    function scheduleAutoScroll() {
      if (idleTimeout !== null) clearTimeout(idleTimeout)
      idleTimeout = setTimeout(() => {
        if (isActive()) startAutoScroll()
      }, IDLE_THRESHOLD_MS)
    }

    function onScroll() {
      applyProgress(getProgress())
      // User-initiated scroll cancels autoscroll and resets idle timer.
      // Programmatic scrollBy from startAutoScroll is ignored here.
      if (!isAutoScrolling) {
        stopAutoScroll()
        scheduleAutoScroll()
      }
    }

    window.addEventListener('scroll', onScroll, { passive: true })
    applyProgress(getProgress())  // sync initial state
    scheduleAutoScroll()

    return () => {
      window.removeEventListener('scroll', onScroll)
      stopAutoScroll()
      if (idleTimeout !== null) clearTimeout(idleTimeout)
    }
  }, [reducedMotion])

  function handleClick(index: number) {
    if (index === activeIndex) return
    setActiveIndex(index)
    setProgressPct(0)
  }

  return (
    <section ref={sectionRef} className="auto-advance-progress">
      <div className="aap-inner">
        <div className="aap-left">
          {BLOCKS.map((block, i) => (
            <div
              key={i}
              className={`aap-block${i === activeIndex ? ' aap-block--active' : ''}`}
              onClick={() => handleClick(i)}
            >
              <div className="aap-block-header">
                <span className="aap-block-tag">{block.tag}</span>
                <span className="aap-block-headline">{block.headline}</span>
              </div>
              <div className="aap-block-body">
                <p>{block.body}</p>
                {i === activeIndex && (
                  <div className="aap-progress-bar">
                    <div className="aap-progress-fill" style={{ width: `${progressPct}%` }} />
                  </div>
                )}
              </div>
            </div>
          ))}
        </div>
        <div className="aap-right">
          {!reducedMotion && (
            <canvas ref={canvasRef} className="aap-canvas" width={300} height={240} />
          )}
        </div>
      </div>
    </section>
  )
}
