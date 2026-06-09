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

// ── ASCII background: domain-warped fbm fluid ────────────────────────────────
// Three-octave trig fbm composed with itself twice (Quilez domain warping)
// produces organic fluid motion — patterns that swirl, merge and dissipate.
const ASCII_CHAR_RAMP    = '   ♠♣♥♦'
const ASCII_PALETTE      = ['#ababab', '#c7c7c7', '#e86b5d', '#90c4a7']
const ASCII_FONT_SIZE    = 15
const ASCII_CELL_SPACING = 1.05
const ASCII_BASE_OPACITY = 0.33
const ASCII_SPOT_OPACITY = 0.95
const ASCII_SPOT_RADIUS  = 9
const ASCII_RIPPLE_STR   = 1.3
const ASCII_RIPPLE_RAD   = 5
const ASCII_FRAME_MS     = 22
const ASCII_FADE_PX      = 200

function asciiFbm(x: number, y: number, t: number): number {
  let v = 0, amp = 0.5, fx = 1, fy = 1
  for (let i = 0; i < 3; i++) {
    v += amp * Math.sin(x * fx * 0.14 + y * fy * 0.11 + t * 0.90 + i * 1.7)
    v += amp * Math.cos(x * fx * 0.09 - y * fy * 0.13 - t * 0.65 + i * 2.3)
    amp *= 0.5; fx *= 2.1; fy *= 1.9
  }
  return v * 0.34
}

function asciiFluidV(x: number, y: number, t: number): number {
  const qx = asciiFbm(x,       y,       t)
  const qy = asciiFbm(x + 3.2, y + 1.7, t + 0.5)
  return asciiFbm(x + 1.5 * qx, y + 1.5 * qy, t + 1.0)
}

function asciiPanelBoundaryX(py: number, rh: number, rw: number): number {
  const ny   = py / rh
  const bell = Math.max(0, 1 - Math.pow((ny - 0.5) / 0.28, 4))
  const base = 0.22 + 0.22 * bell
  const shore = 0.038 * Math.sin(py * 0.034 + 1.1)
              + 0.022 * Math.sin(py * 0.071 - 0.5)
              + 0.013 * Math.sin(py * 0.138 + 2.8)
  return (base + shore) * rw
}

// Top/bottom shoreline boundaries — vary by x so the edge is organic, not a flat line
function asciiTopBoundaryY(px: number, rh: number): number {
  const base  = 0.1
  const shore = 0.018 * Math.sin(px * 0.018 + 2.0)
              + 0.010 * Math.sin(px * 0.034 - 1.1)
  return (base + shore) * rh
}

function asciiBottomBoundaryY(px: number, rh: number): number {
  return rh - asciiTopBoundaryY(px, rh)
}

const ASCII_VERTICAL_FADE_PX = 150

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
  const asciiCanvasRef = useRef<HTMLCanvasElement>(null)
  const asciiMouseRef = useRef({ x: -9999, y: -9999 })

  const triggerAnim = useCallback((index: number) => {
    const state = chipStateRef.current
    const anim = buildAnim(index, state.positions)
    anim.onComplete = () => {
      chipStateRef.current.positions = SEAT_POSITIONS.map(p => ({ ...p }))
      chipStateRef.current.anim = null
    }
    state.anim = anim
  }, [])

  // ASCII fluid background
  useEffect(() => {
    if (reducedMotion) return
    const canvas = asciiCanvasRef.current
    if (!canvas) return
    const ctx = canvas.getContext('2d')
    if (!ctx) return

    let cols = 0, rows = 0, cellW = 0, cellH = 0, dpr = 1, lastFrame = 0
    let asciiRaf = 0

    const resize = () => {
      dpr = Math.min(window.devicePixelRatio || 1, 2)
      canvas.width  = Math.floor(window.innerWidth  * dpr)
      canvas.height = Math.floor(window.innerHeight * dpr)
      ctx.setTransform(dpr, 0, 0, dpr, 0, 0)
      ctx.font = `${ASCII_FONT_SIZE}px Arial, sans-serif`
      ctx.textBaseline = 'top'
      const m = ctx.measureText('M')
      cellW = (m.width || ASCII_FONT_SIZE * 0.6) * ASCII_CELL_SPACING
      cellH = ASCII_FONT_SIZE * 1.15 * ASCII_CELL_SPACING
      cols  = Math.max(1, Math.floor(window.innerWidth  / cellW))
      rows  = Math.max(1, Math.floor(window.innerHeight / cellH))
    }
    resize()

    const onMouseMove = (e: MouseEvent) => { asciiMouseRef.current = { x: e.clientX, y: e.clientY } }
    window.addEventListener('mousemove', onMouseMove, { passive: true })
    window.addEventListener('resize', resize)

    const rampMax = ASCII_CHAR_RAMP.length - 1
    const spotR2  = ASCII_SPOT_RADIUS * ASCII_SPOT_RADIUS * 2
    const chipHW  = 150, chipHH = 120

    const draw = (t: number) => {
      if (t - lastFrame < ASCII_FRAME_MS) { asciiRaf = requestAnimationFrame(draw); return }
      lastFrame = t
      const time = t * 0.0018
      const W = window.innerWidth, H = window.innerHeight
      const rect = canvas.getBoundingClientRect()
      const mx = asciiMouseRef.current.x, my = asciiMouseRef.current.y
      const cx = (mx - rect.left) / cellW, cy = (my - rect.top) / cellH
      const margin = 24
      const inside = mx >= rect.left - margin && mx <= rect.right  + margin
                  && my >= rect.top  - margin && my <= rect.bottom + margin
      const chipCX = W * 0.72, chipCY = H * 0.5

      ctx.clearRect(0, 0, W, H)
      for (let y = 0; y < rows; y++) {
        for (let x = 0; x < cols; x++) {
          const px = x * cellW, py = y * cellH
          const boundary  = asciiPanelBoundaryX(py, H, W)
          const fadePanel = Math.max(0, Math.min(1, (px - boundary) / ASCII_FADE_PX))
          const dxC = Math.max(0, Math.abs(px - chipCX) - chipHW)
          const dyC = Math.max(0, Math.abs(py - chipCY) - chipHH)
          const fadeChip  = Math.min(1, Math.sqrt(dxC * dxC + dyC * dyC) / ASCII_FADE_PX)
          const fadeTop    = Math.max(0, Math.min(1, (py - asciiTopBoundaryY(px, H))    / ASCII_VERTICAL_FADE_PX))
          const fadeBottom = Math.max(0, Math.min(1, (asciiBottomBoundaryY(px, H) - py) / ASCII_VERTICAL_FADE_PX))
          const zoneFade   = Math.min(fadePanel, fadeChip, fadeTop, fadeBottom)
          if (zoneFade <= 0.01) continue

          const fluid  = asciiFluidV(x * 0.28, y * 0.28, time)
          const dx = x - cx, dy = (y - cy) * 1.8, d2 = dx * dx + dy * dy, d = Math.sqrt(d2)
          const ripple = inside
            ? ASCII_RIPPLE_STR * Math.exp(-d2 / 80) - 0.6 * Math.exp(-((d - ASCII_RIPPLE_RAD) ** 2) / 30)
            : 0
          const v  = Math.max(0, Math.min(1, 0.5 + fluid * 0.55 + ripple * 0.3))
          const ch = ASCII_CHAR_RAMP[Math.floor(v * rampMax)]
          if (ch === ' ') continue

          let alpha = ASCII_BASE_OPACITY
          if (inside) {
            alpha = ASCII_BASE_OPACITY + (ASCII_SPOT_OPACITY - ASCII_BASE_OPACITY) * Math.exp(-d2 / spotR2)
            alpha = Math.max(0, Math.min(1, alpha))
          }
          alpha *= zoneFade
          if (alpha <= 0.01) continue

          const huePos = (x * 0.1 + y * 0.07 + time * 0.12) % ASCII_PALETTE.length
          ctx.globalAlpha = alpha
          ctx.fillStyle   = ASCII_PALETTE[Math.floor(Math.abs(huePos)) % ASCII_PALETTE.length]
          ctx.fillText(ch, px, py)
        }
      }
      ctx.globalAlpha = 1
      asciiRaf = requestAnimationFrame(draw)
    }
    asciiRaf = requestAnimationFrame(draw)

    return () => {
      cancelAnimationFrame(asciiRaf)
      window.removeEventListener('mousemove', onMouseMove)
      window.removeEventListener('resize', resize)
    }
  }, [reducedMotion])

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
        {!reducedMotion && <canvas ref={asciiCanvasRef} className="aap-ascii-canvas" />}
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
