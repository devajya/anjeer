import { useEffect, useRef } from 'react'
import './ScrollTrackerSection.css'

export interface ScrollTrackerSectionProps {
  reducedMotion: boolean
}

// Frame geometry as viewport fractions — mirrored by CSS vw/vh values below.
// Changing these constants requires updating ScrollTrackerSection.css too.
const F = {
  left:     0.38,   // frame left edge   (38vw)
  top:      0.09,   // frame top edge    (9vh)
  right:    0.96,   // frame right edge  (96vw → 4vw from right)
  bottom:   0.91,   // frame bottom edge (91vh)
  cardLeft: 0.66,   // notch left edge   (66vw)
  cardTop:  0.70,   // notch top edge    (70vh)
}

const INDICATOR_W = 38
const INDICATOR_H = 16

// ── Path-walking utilities ──────────────────────────────────────────────────

interface Segment { x1: number; y1: number; x2: number; y2: number; len: number; angle: number }

function buildSegments(fl: number, ft: number, fr: number, fb: number, fcl: number, fct: number, W: number): Segment[] {
  // Open path: left screen edge → stepped frame shape → right screen edge
  const pts: [number, number][] = [
    [0, ft], [fl, ft], [fl, fb], [fcl, fb], [fcl, fct], [fr, fct], [fr, ft], [W, ft],
  ]
  return pts.slice(0, -1).map((p, i) => {
    const [x1, y1] = p
    const [x2, y2] = pts[i + 1]
    return { x1, y1, x2, y2, len: Math.hypot(x2 - x1, y2 - y1), angle: Math.atan2(y2 - y1, x2 - x1) * 180 / Math.PI }
  })
}

function getPointOnPath(segs: Segment[], progress: number): { x: number; y: number; angle: number } {
  const total = segs.reduce((s, seg) => s + seg.len, 0)
  let target = progress * total
  for (const seg of segs) {
    if (target <= seg.len) {
      const t = target / seg.len
      return { x: seg.x1 + t * (seg.x2 - seg.x1), y: seg.y1 + t * (seg.y2 - seg.y1), angle: seg.angle }
    }
    target -= seg.len
  }
  const last = segs[segs.length - 1]
  return { x: last.x2, y: last.y2, angle: last.angle }
}
const SUITS = ['♠', '♣', '♥', '♦'] as const
const N = 4
const FADE_WIDTH = 0.07

function stateOpacity(progress: number, i: number): number {
  const lo = i / N
  const hi = (i + 1) / N
  if (i === 0 && progress <= lo) return 1
  if (i === N - 1 && progress >= hi) return 1
  if (progress < lo - FADE_WIDTH || progress > hi + FADE_WIDTH) return 0
  const fadeIn  = (progress - (lo - FADE_WIDTH)) / (2 * FADE_WIDTH)
  const fadeOut = ((hi + FADE_WIDTH) - progress) / (2 * FADE_WIDTH)
  return Math.max(0, Math.min(1, Math.min(fadeIn, fadeOut)))
}

// ── SVG illustrations ──────────────────────────────────────────────────────

function BrowserSVG() {
  return (
    <svg width="200" height="200" viewBox="0 0 200 200" fill="none" aria-hidden="true" className="scroll-tracker-svg">
      <rect x="20" y="30" width="160" height="140" rx="4" stroke="currentColor" strokeWidth="1.5" />
      <line x1="20" y1="55" x2="180" y2="55" stroke="currentColor" strokeWidth="1.5" />
      <circle cx="38" cy="43" r="5" stroke="currentColor" strokeWidth="1.5" />
      <circle cx="56" cy="43" r="5" stroke="currentColor" strokeWidth="1.5" />
      <circle cx="74" cy="43" r="5" stroke="currentColor" strokeWidth="1.5" />
      <rect x="90" y="36" width="72" height="13" rx="2" stroke="currentColor" strokeWidth="1" />
      <rect x="32" y="72" width="60" height="8" rx="1" fill="currentColor" opacity="0.3" />
      <rect x="32" y="88" width="136" height="6" rx="1" fill="currentColor" opacity="0.15" />
      <rect x="32" y="100" width="120" height="6" rx="1" fill="currentColor" opacity="0.15" />
      <rect x="32" y="112" width="90" height="6" rx="1" fill="currentColor" opacity="0.15" />
      <rect x="32" y="130" width="40" height="22" rx="2" stroke="currentColor" strokeWidth="1.5" />
      <rect x="80" y="130" width="40" height="22" rx="2" stroke="currentColor" strokeWidth="1.5" />
    </svg>
  )
}

function TerminalSVG() {
  return (
    <svg width="200" height="200" viewBox="0 0 200 200" fill="none" aria-hidden="true" className="scroll-tracker-svg">
      <rect x="20" y="30" width="160" height="140" rx="4" stroke="currentColor" strokeWidth="1.5" />
      <line x1="20" y1="52" x2="180" y2="52" stroke="currentColor" strokeWidth="1.5" />
      <circle cx="35" cy="41" r="4" stroke="currentColor" strokeWidth="1.5" />
      <circle cx="50" cy="41" r="4" stroke="currentColor" strokeWidth="1.5" />
      <circle cx="65" cy="41" r="4" stroke="currentColor" strokeWidth="1.5" />
      <text x="32" y="75" fontFamily="monospace" fontSize="11" fill="currentColor" opacity="0.7">$ anjeer join</text>
      <text x="32" y="93" fontFamily="monospace" fontSize="11" fill="currentColor" opacity="0.5">Connecting...</text>
      <text x="32" y="111" fontFamily="monospace" fontSize="11" fill="currentColor" opacity="0.7">$ place bid 42</text>
      <text x="32" y="129" fontFamily="monospace" fontSize="11" fill="currentColor" opacity="0.5">Order placed.</text>
      <text x="32" y="153" fontFamily="monospace" fontSize="11" fill="currentColor" opacity="0.9">$ _</text>
    </svg>
  )
}

function SpectatorSVG() {
  return (
    <svg width="200" height="200" viewBox="0 0 200 200" fill="none" aria-hidden="true" className="scroll-tracker-svg">
      {/* Window chrome */}
      <rect x="20" y="30" width="160" height="140" rx="4" stroke="currentColor" strokeWidth="1.5" />
      <line x1="20" y1="52" x2="180" y2="52" stroke="currentColor" strokeWidth="1.5" />
      <circle cx="35" cy="41" r="4" stroke="currentColor" strokeWidth="1.5" />
      <circle cx="50" cy="41" r="4" stroke="currentColor" strokeWidth="1.5" />
      <circle cx="65" cy="41" r="4" stroke="currentColor" strokeWidth="1.5" />
      {/* LIVE pill */}
      <rect x="142" y="35" width="26" height="12" rx="6" fill="currentColor" opacity="0.18" />
      <text x="155" y="44" fontFamily="monospace" fontSize="7" fill="currentColor" opacity="0.7" textAnchor="middle">LIVE</text>
      {/* Column headers */}
      <text x="56" y="68" fontFamily="monospace" fontSize="8" fill="currentColor" opacity="0.45" textAnchor="middle">BID</text>
      <text x="144" y="68" fontFamily="monospace" fontSize="8" fill="currentColor" opacity="0.45" textAnchor="middle">ASK</text>
      {/* Center divider */}
      <line x1="100" y1="58" x2="100" y2="162" stroke="currentColor" strokeWidth="0.75" opacity="0.2" />
      {/* Bid bars — right-aligned to x=97, best bid widest */}
      <rect x="47" y="74"  width="50" height="7" rx="1" fill="currentColor" opacity="0.55" />
      <rect x="53" y="85"  width="44" height="7" rx="1" fill="currentColor" opacity="0.4" />
      <rect x="61" y="96"  width="36" height="7" rx="1" fill="currentColor" opacity="0.28" />
      <rect x="69" y="107" width="28" height="7" rx="1" fill="currentColor" opacity="0.18" />
      <rect x="75" y="118" width="22" height="7" rx="1" fill="currentColor" opacity="0.1" />
      {/* Ask bars — left-aligned from x=103 */}
      <rect x="103" y="74"  width="50" height="7" rx="1" fill="currentColor" opacity="0.55" />
      <rect x="103" y="85"  width="44" height="7" rx="1" fill="currentColor" opacity="0.4" />
      <rect x="103" y="96"  width="36" height="7" rx="1" fill="currentColor" opacity="0.28" />
      <rect x="103" y="107" width="28" height="7" rx="1" fill="currentColor" opacity="0.18" />
      <rect x="103" y="118" width="22" height="7" rx="1" fill="currentColor" opacity="0.1" />
      {/* Spread label */}
      <text x="100" y="142" fontFamily="monospace" fontSize="7" fill="currentColor" opacity="0.35" textAnchor="middle">spread: 1</text>
      {/* Bottom status bar */}
      <line x1="20" y1="152" x2="180" y2="152" stroke="currentColor" strokeWidth="0.75" opacity="0.2" />
      <text x="32" y="161" fontFamily="monospace" fontSize="7" fill="currentColor" opacity="0.35">read-only</text>
    </svg>
  )
}

function BotsSVG() {
  return (
    <svg width="200" height="200" viewBox="0 0 200 200" fill="none" aria-hidden="true" className="scroll-tracker-svg">
      {/* Window chrome */}
      <rect x="20" y="30" width="160" height="140" rx="4" stroke="currentColor" strokeWidth="1.5" />
      <line x1="20" y1="52" x2="180" y2="52" stroke="currentColor" strokeWidth="1.5" />
      <circle cx="35" cy="41" r="4" stroke="currentColor" strokeWidth="1.5" />
      <circle cx="50" cy="41" r="4" stroke="currentColor" strokeWidth="1.5" />
      <circle cx="65" cy="41" r="4" stroke="currentColor" strokeWidth="1.5" />
      {/* EASY row */}
      <rect x="32" y="64" width="136" height="22" rx="2" stroke="currentColor" strokeWidth="1" opacity="0.3" />
      <circle cx="46" cy="75" r="5" fill="currentColor" opacity="0.25" />
      <text x="58" y="79" fontFamily="monospace" fontSize="9" fill="currentColor" opacity="0.6">EASY</text>
      <rect x="120" y="69" width="12" height="4" rx="1" fill="currentColor" opacity="0.2" />
      <rect x="134" y="69" width="12" height="4" rx="1" fill="currentColor" opacity="0.2" />
      <rect x="148" y="69" width="8"  height="4" rx="1" fill="currentColor" opacity="0.2" />
      <rect x="120" y="75" width="8"  height="4" rx="1" fill="currentColor" opacity="0.2" />
      <rect x="130" y="75" width="14" height="4" rx="1" fill="currentColor" opacity="0.2" />
      {/* MEDIUM row */}
      <rect x="32" y="96" width="136" height="22" rx="2" stroke="currentColor" strokeWidth="1.5" opacity="0.7" />
      <circle cx="46" cy="107" r="5" fill="currentColor" opacity="0.55" />
      <text x="58" y="111" fontFamily="monospace" fontSize="9" fill="currentColor" opacity="0.85">MEDIUM</text>
      <rect x="120" y="101" width="12" height="4" rx="1" fill="currentColor" opacity="0.45" />
      <rect x="134" y="101" width="8"  height="4" rx="1" fill="currentColor" opacity="0.45" />
      <rect x="144" y="101" width="14" height="4" rx="1" fill="currentColor" opacity="0.45" />
      <rect x="120" y="107" width="14" height="4" rx="1" fill="currentColor" opacity="0.45" />
      <rect x="136" y="107" width="10" height="4" rx="1" fill="currentColor" opacity="0.45" />
      {/* HARD row */}
      <rect x="32" y="128" width="136" height="22" rx="2" stroke="currentColor" strokeWidth="1" opacity="0.3" />
      <circle cx="46" cy="139" r="5" fill="currentColor" opacity="0.25" />
      <text x="58" y="143" fontFamily="monospace" fontSize="9" fill="currentColor" opacity="0.6">HARD</text>
      <rect x="120" y="133" width="14" height="4" rx="1" fill="currentColor" opacity="0.2" />
      <rect x="136" y="133" width="10" height="4" rx="1" fill="currentColor" opacity="0.2" />
      <rect x="148" y="133" width="8"  height="4" rx="1" fill="currentColor" opacity="0.2" />
      <rect x="120" y="139" width="10" height="4" rx="1" fill="currentColor" opacity="0.2" />
      <rect x="132" y="139" width="14" height="4" rx="1" fill="currentColor" opacity="0.2" />
    </svg>
  )
}

const SUB_SECTIONS = [
  {
    number: '01',
    eyebrow: 'In the browser',
    headline: 'Trade in your browser',
    body: 'No installs. Open a lobby, grab a seat, and start trading in seconds from any modern browser.',
    svg: <BrowserSVG />,
  },
  {
    number: '02',
    eyebrow: 'From the terminal',
    headline: 'Script from the terminal',
    body: 'Use the CLI tool to join games programmatically. Pipe your strategy straight into the market.',
    svg: <TerminalSVG />,
  },
  {
    number: '03',
    eyebrow: 'Read-only access',
    headline: 'Spectate any game',
    body: 'Watch live order flow, delta tables, and eval signals from any seat without touching a hand.',
    svg: <SpectatorSVG />,
  },
  {
    number: '04',
    eyebrow: 'Always a full table',
    headline: 'Play against bots',
    body: 'Three difficulty tiers — Easy, Medium, Hard. Bots fill empty seats so games always run.',
    svg: <BotsSVG />,
  },
]

// ── Component ──────────────────────────────────────────────────────────────

export function ScrollTrackerSection({ reducedMotion }: ScrollTrackerSectionProps) {
  const sectionRef  = useRef<HTMLElement>(null)
  const stickyRef   = useRef<HTMLDivElement>(null)
  const framePathRef = useRef<SVGPathElement>(null)
  const indicatorRef = useRef<HTMLDivElement>(null)
  const symbolRef   = useRef<HTMLSpanElement>(null)
  const illustRefs  = useRef<(HTMLDivElement | null)[]>([])
  const contentRefs = useRef<(HTMLDivElement | null)[]>([])
  const cardRefs    = useRef<(HTMLDivElement | null)[]>([])
  const segsRef     = useRef<Segment[]>([])

  useEffect(() => {
    if (reducedMotion) return

    // Initial opacity: state 0 visible, rest hidden
    ;[illustRefs, contentRefs, cardRefs].forEach(group => {
      group.current.forEach((el, i) => { if (el) el.style.opacity = i === 0 ? '1' : '0' })
    })

    // Build the stepped frame path and recompute path segments from live viewport dims.
    // Called on mount and on every resize.
    function buildFrame() {
      const W = window.innerWidth
      const H = window.innerHeight
      const fl = W * F.left,  ft = H * F.top
      const fr = W * F.right, fb = H * F.bottom
      const fcl = W * F.cardLeft, fct = H * F.cardTop

      const d = `M 0,${ft} L ${fl},${ft} L ${fl},${fb} L ${fcl},${fb} L ${fcl},${fct} L ${fr},${fct} L ${fr},${ft} L ${W},${ft}`
      framePathRef.current?.setAttribute('d', d)

      segsRef.current = buildSegments(fl, ft, fr, fb, fcl, fct, W)

      // Place indicator at progress=0
      const pt = getPointOnPath(segsRef.current, 0)
      if (indicatorRef.current) {
        indicatorRef.current.style.left      = `${pt.x - INDICATOR_W / 2}px`
        indicatorRef.current.style.top       = `${pt.y - INDICATOR_H / 2}px`
        indicatorRef.current.style.transform = `rotate(${pt.angle}deg)`
      }
    }

    buildFrame()

    const ro = new ResizeObserver(() => buildFrame())
    if (stickyRef.current) ro.observe(stickyRef.current)

    let st: { kill(): void } | null = null

    import('gsap').then(({ gsap }) => {
      import('gsap/ScrollTrigger').then(({ ScrollTrigger }) => {
        gsap.registerPlugin(ScrollTrigger)
        if (!sectionRef.current) return

        st = ScrollTrigger.create({
          trigger: sectionRef.current,
          start: 'top top',
          end: 'bottom bottom',
          scrub: 0.4,
          onUpdate: ({ progress }: { progress: number }) => {
            // Walk indicator along the full stepped frame path
            const pt = getPointOnPath(segsRef.current, progress)
            if (indicatorRef.current) {
              indicatorRef.current.style.left      = `${pt.x - INDICATOR_W / 2}px`
              indicatorRef.current.style.top       = `${pt.y - INDICATOR_H / 2}px`
              indicatorRef.current.style.transform = `rotate(${pt.angle}deg)`
            }

            // Counter-rotate symbol so it stays upright
            if (symbolRef.current) {
              symbolRef.current.textContent = SUITS[Math.min(N - 1, Math.floor(progress * N))]
              symbolRef.current.style.transform = `rotate(${-pt.angle}deg)`
            }

            // Cross-fade all three element groups
            ;[illustRefs, contentRefs, cardRefs].forEach(group => {
              group.current.forEach((el, i) => {
                if (el) el.style.opacity = String(stateOpacity(progress, i))
              })
            })
          },
        })
      })
    }).catch(() => {})

    return () => {
      st?.kill()
      ro.disconnect()
    }
  }, [reducedMotion])

  // ── Reduced-motion fallback ──────────────────────────────────────────────
  if (reducedMotion) {
    return (
      <section className="scroll-tracker-section scroll-tracker-section--static" aria-label="Platform features">
        <div className="scroll-tracker-static-grid">
          {SUB_SECTIONS.map((s) => (
            <div key={s.number} className="scroll-tracker-static-card">
              <div className="scroll-tracker-static-svg">{s.svg}</div>
              <span className="scroll-tracker-eyebrow">{s.eyebrow}</span>
              <h3 className="scroll-tracker-headline">{s.headline}</h3>
              <p className="scroll-tracker-body">{s.body}</p>
              <span className="scroll-tracker-number">{s.number}</span>
            </div>
          ))}
        </div>
      </section>
    )
  }

  // ── Animated (full) path ─────────────────────────────────────────────────
  return (
    <section
      ref={sectionRef}
      className="scroll-tracker-section"
      aria-label="Platform features"
    >
      <div ref={stickyRef} className="scroll-tracker-sticky">

        {/* SVG frame: stepped border, pointer-events off so it never blocks clicks */}
        <svg className="scroll-tracker-frame-svg" aria-hidden="true">
          <path ref={framePathRef} className="scroll-tracker-frame-path" />
        </svg>

        {/* Indicator: pill + suit symbol, position driven by JS */}
        <div ref={indicatorRef} className="scroll-tracker-indicator" aria-hidden="true">
          <span ref={symbolRef} className="scroll-tracker-indicator-symbol">♠</span>
        </div>

        {/* Illustration column — left of frame, 4 states cross-fade */}
        <div className="scroll-tracker-illust-col">
          {SUB_SECTIONS.map((s, i) => (
            <div
              key={s.number}
              ref={(el) => { illustRefs.current[i] = el }}
              className="scroll-tracker-illust"
              style={{ opacity: i === 0 ? 1 : 0 }}
            >
              {s.svg}
            </div>
          ))}
        </div>

        {/* Content inside frame — eyebrow + headline, 4 states cross-fade */}
        <div className="scroll-tracker-content-col">
          {SUB_SECTIONS.map((s, i) => (
            <div
              key={s.number}
              ref={(el) => { contentRefs.current[i] = el }}
              className="scroll-tracker-content"
              style={{ opacity: i === 0 ? 1 : 0 }}
            >
              <span className="scroll-tracker-eyebrow">{s.eyebrow}</span>
              <h3 className="scroll-tracker-headline">{s.headline}</h3>
              <p className="scroll-tracker-body">{s.body}</p>
            </div>
          ))}
        </div>

        {/* Numbered card in notch — 4 states cross-fade */}
        <div className="scroll-tracker-card-col">
          {SUB_SECTIONS.map((s, i) => (
            <div
              key={s.number}
              ref={(el) => { cardRefs.current[i] = el }}
              className="scroll-tracker-number-card"
              style={{ opacity: i === 0 ? 1 : 0 }}
            >
              <span className="scroll-tracker-number">{s.number}</span>
              <p className="scroll-tracker-card-body">{s.body}</p>
            </div>
          ))}
        </div>

      </div>
    </section>
  )
}
