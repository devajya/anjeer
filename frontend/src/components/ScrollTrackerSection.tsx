import { useEffect, useRef } from 'react'
import './ScrollTrackerSection.css'

export interface ScrollTrackerSectionProps {
  reducedMotion: boolean
}

// SVG illustrations for each sub-section
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
      <text x="32" y="153" fontFamily="monospace" fontSize="11" fill="currentColor" opacity="0.9">$ <tspan className="cursor">_</tspan></text>
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
    headline: 'Trade in your browser',
    body: 'No installs. Open a lobby, grab a seat, and start trading in seconds from any modern browser.',
    svg: <BrowserSVG />,
  },
  {
    number: '02',
    headline: 'Script from the terminal',
    body: 'Use the CLI tool to join games programmatically. Pipe your strategy straight into the market.',
    svg: <TerminalSVG />,
  },
  {
    number: '03',
    headline: 'Spectate any game',
    body: 'Watch live order flow, delta tables, and eval signals from any seat without touching a hand.',
    svg: <EyeSVG />,
  },
  {
    number: '04',
    headline: 'Play against bots',
    body: 'Three difficulty tiers — Easy, Medium, Hard. Bots fill empty seats so games always run.',
    svg: <RobotSVG />,
  },
]

export function ScrollTrackerSection({ reducedMotion }: ScrollTrackerSectionProps) {
  const sectionRef = useRef<HTMLElement>(null)
  const pillRef = useRef<HTMLDivElement>(null)

  useEffect(() => {
    // Skip GSAP init for reduced motion — pill stays at left: 0
    if (reducedMotion) return

    let gsapInstance: typeof import('gsap') | null = null
    let ScrollTriggerPlugin: unknown = null

    import('gsap').then(({ gsap }) => {
      return import('gsap/ScrollTrigger').then(({ ScrollTrigger }) => {
        gsapInstance = { gsap } as unknown as typeof import('gsap')
        ScrollTriggerPlugin = ScrollTrigger
        gsap.registerPlugin(ScrollTrigger)

        if (!sectionRef.current || !pillRef.current) return

        gsap.to(pillRef.current, {
          left: 'calc(100% - 48px)',
          ease: 'none',
          scrollTrigger: {
            trigger: sectionRef.current,
            start: 'top top',
            end: 'bottom bottom',
            scrub: true,
          },
        })
      })
    }).catch(() => {
      // AGENT-CTX: GSAP import failure is non-fatal — pill stays at initial position.
    })

    return () => {
      if (ScrollTriggerPlugin) {
        // Clean up ScrollTrigger instances on unmount to avoid memory leaks
        (ScrollTriggerPlugin as { getAll(): { kill(): void }[] }).getAll().forEach((t) => t.kill())
      }
    }
  }, [reducedMotion])

  return (
    <section
      ref={sectionRef}
      className="scroll-tracker-section"
      aria-label="Platform features"
    >
      <div className="scroll-tracker-pill-track">
        <div
          ref={pillRef}
          className="scroll-tracker-pill"
          style={reducedMotion ? { left: '0px' } : undefined}
        />
      </div>

      <div className="scroll-tracker-inner">
        {SUB_SECTIONS.map((s) => (
          <div key={s.number} className="scroll-tracker-subsection">
            <div className="scroll-tracker-svg-col">{s.svg}</div>
            <div className="scroll-tracker-content">
              <h3 className="scroll-tracker-headline">{s.headline}</h3>
              <p className="scroll-tracker-body">{s.body}</p>
            </div>
            <div className="scroll-tracker-number-card">
              <span className="scroll-tracker-number">{s.number}</span>
            </div>
          </div>
        ))}
      </div>
    </section>
  )
}
