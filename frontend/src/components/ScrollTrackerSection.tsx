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

function EyeSVG() {
  return (
    <svg width="200" height="200" viewBox="0 0 200 200" fill="none" aria-hidden="true" className="scroll-tracker-svg">
      <path d="M20 100 C60 50 140 50 180 100 C140 150 60 150 20 100Z" stroke="currentColor" strokeWidth="1.5" />
      <circle cx="100" cy="100" r="28" stroke="currentColor" strokeWidth="1.5" />
      <circle cx="100" cy="100" r="14" fill="currentColor" opacity="0.25" />
      <circle cx="100" cy="100" r="6" fill="currentColor" opacity="0.6" />
      <line x1="100" y1="60" x2="100" y2="45" stroke="currentColor" strokeWidth="1" opacity="0.4" />
      <line x1="100" y1="140" x2="100" y2="155" stroke="currentColor" strokeWidth="1" opacity="0.4" />
      <line x1="60" y1="100" x2="45" y2="100" stroke="currentColor" strokeWidth="1" opacity="0.4" />
      <line x1="140" y1="100" x2="155" y2="100" stroke="currentColor" strokeWidth="1" opacity="0.4" />
    </svg>
  )
}

function RobotSVG() {
  return (
    <svg width="200" height="200" viewBox="0 0 200 200" fill="none" aria-hidden="true" className="scroll-tracker-svg">
      <rect x="65" y="55" width="70" height="60" rx="6" stroke="currentColor" strokeWidth="1.5" />
      <rect x="80" y="70" width="16" height="12" rx="2" stroke="currentColor" strokeWidth="1.5" />
      <rect x="104" y="70" width="16" height="12" rx="2" stroke="currentColor" strokeWidth="1.5" />
      <line x1="100" y1="55" x2="100" y2="42" stroke="currentColor" strokeWidth="1.5" />
      <circle cx="100" cy="38" r="5" stroke="currentColor" strokeWidth="1.5" />
      <line x1="80" y1="92" x2="112" y2="92" stroke="currentColor" strokeWidth="1" opacity="0.5" />
      <rect x="70" y="115" width="60" height="45" rx="4" stroke="currentColor" strokeWidth="1.5" />
      <line x1="65" y1="125" x2="50" y2="125" stroke="currentColor" strokeWidth="1.5" />
      <line x1="50" y1="125" x2="50" y2="148" stroke="currentColor" strokeWidth="1.5" />
      <line x1="135" y1="125" x2="150" y2="125" stroke="currentColor" strokeWidth="1.5" />
      <line x1="150" y1="125" x2="150" y2="148" stroke="currentColor" strokeWidth="1.5" />
      <line x1="80" y1="160" x2="80" y2="178" stroke="currentColor" strokeWidth="1.5" />
      <line x1="120" y1="160" x2="120" y2="178" stroke="currentColor" strokeWidth="1.5" />
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
    svg: <EyeSVG />,
  },
  {
    number: '04',
    eyebrow: 'Always a full table',
    headline: 'Play against bots',
    body: 'Three difficulty tiers — Easy, Medium, Hard. Bots fill empty seats so games always run.',
    svg: <RobotSVG />,
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

  useEffect(() => {
    if (reducedMotion) return

    // Initial opacity: state 0 visible, rest hidden
    ;[illustRefs, contentRefs, cardRefs].forEach(group => {
      group.current.forEach((el, i) => { if (el) el.style.opacity = i === 0 ? '1' : '0' })
    })

    // Build the stepped frame path and indicator start position from live viewport dims.
    // Called on mount and on every resize.
    function buildFrame() {
      const W = window.innerWidth
      const H = window.innerHeight
      const fl = W * F.left,  ft = H * F.top
      const fr = W * F.right, fb = H * F.bottom
      const fcl = W * F.cardLeft, fct = H * F.cardTop

      // Stepped border: full frame with rectangular notch cut from bottom-right for the card
      const d = `M ${fl},${ft} L ${fr},${ft} L ${fr},${fct} L ${fcl},${fct} L ${fcl},${fb} L ${fl},${fb} Z`
      framePathRef.current?.setAttribute('d', d)

      // Place indicator at progress=0 (left edge of top border)
      if (indicatorRef.current) {
        indicatorRef.current.style.left = `${fl}px`
        indicatorRef.current.style.top  = `${ft - INDICATOR_H / 2}px`
      }

      return { fl, ft, fr }
    }

    let dims = buildFrame()

    const ro = new ResizeObserver(() => { dims = buildFrame() })
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
            const { fl, ft, fr } = dims

            // Slide indicator along the top border, left → right
            const travel = fr - fl - INDICATOR_W
            if (indicatorRef.current) {
              indicatorRef.current.style.left = `${fl + progress * travel}px`
              indicatorRef.current.style.top  = `${ft - INDICATOR_H / 2}px`
            }

            // Suit symbol switches at each state boundary
            if (symbolRef.current) {
              symbolRef.current.textContent = SUITS[Math.min(N - 1, Math.floor(progress * N))]
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
