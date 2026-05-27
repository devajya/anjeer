import { useEffect, useRef } from 'react'
import './MathDive.css'

export interface MathDiveProps {
  reducedMotion: boolean
}

// ── Canvas draw functions ──────────────────────────────────────────────────

function drawEV(ctx: CanvasRenderingContext2D, t: number, w: number, h: number) {
  ctx.clearRect(0, 0, w, h)

  const suits    = ['♠', '♣', '♥', '♦'] as const
  const bases    = [0.55, 0.78, 0.42, 0.88]
  const ampls    = [0.07, 0.05, 0.09, 0.04]
  const phases   = [0, 1.4, 2.8, 0.7]
  const barW     = 26
  const gap      = 58
  const startX   = w / 2 - gap * 1.5
  const baseY    = h - 38

  ctx.strokeStyle = 'rgba(240,240,240,0.1)'
  ctx.lineWidth   = 1
  ctx.beginPath()
  ctx.moveTo(startX - 18, baseY)
  ctx.lineTo(startX + gap * 3 + 18, baseY)
  ctx.stroke()

  suits.forEach((suit, i) => {
    const ph = Math.max(0.15, bases[i] + Math.sin(t * 0.6 + phases[i]) * ampls[i])
    const bh = ph * (h - 68)
    const x  = startX + i * gap

    const g = ctx.createLinearGradient(x, baseY - bh, x, baseY)
    g.addColorStop(0, 'rgba(201,168,76,0.95)')
    g.addColorStop(0.6, 'rgba(201,168,76,0.55)')
    g.addColorStop(1, 'rgba(201,168,76,0.08)')
    ctx.fillStyle = g
    ctx.fillRect(x - barW / 2, baseY - bh, barW, bh)

    ctx.fillStyle  = 'rgba(201,168,76,0.8)'
    ctx.font       = '10px "IBM Plex Mono", monospace'
    ctx.textAlign  = 'center'
    ctx.fillText((ph * 100).toFixed(0) + '%', x, baseY - bh - 7)

    ctx.fillStyle = 'rgba(240,240,240,0.4)'
    ctx.font      = '13px serif'
    ctx.fillText(suit, x, baseY + 16)
  })

  ctx.fillStyle = 'rgba(240,240,240,0.16)'
  ctx.font      = '9px "IBM Plex Mono", monospace'
  ctx.textAlign = 'left'
  ctx.fillText('p(suit)', startX - 18, baseY - (h - 68) * 0.5)
}

function drawBayes(ctx: CanvasRenderingContext2D, t: number, w: number, h: number) {
  ctx.clearRect(0, 0, w, h)

  const cx = w / 2
  const cy = h / 2 - 8

  const nodes = [
    { x: cx - 88, y: cy + 14,  label: 'prior',     val: 0.25 + Math.sin(t * 0.35) * 0.04 },
    { x: cx,      y: cy - 46,  label: 'evidence',  val: 0.72 + Math.sin(t * 0.55 + 1.1) * 0.07 },
    { x: cx + 88, y: cy + 14,  label: 'posterior', val: 0.64 + Math.sin(t * 0.42 + 2.3) * 0.06 },
  ]

  const edges: [number, number][] = [[0, 1], [1, 2], [0, 2]]
  edges.forEach(([a, b]) => {
    ctx.strokeStyle = 'rgba(201,168,76,0.18)'
    ctx.lineWidth   = 1
    ctx.beginPath()
    ctx.moveTo(nodes[a].x, nodes[a].y)
    ctx.lineTo(nodes[b].x, nodes[b].y)
    ctx.stroke()
  })

  edges.forEach(([a, b], ei) => {
    const p  = ((t * 0.5 + ei * 0.33) % 1)
    const px = nodes[a].x + (nodes[b].x - nodes[a].x) * p
    const py = nodes[a].y + (nodes[b].y - nodes[a].y) * p
    ctx.fillStyle = `rgba(201,168,76,${(Math.sin(p * Math.PI) * 0.85).toFixed(2)})`
    ctx.beginPath()
    ctx.arc(px, py, 3, 0, Math.PI * 2)
    ctx.fill()
  })

  nodes.forEach(node => {
    ctx.fillStyle = 'rgba(201,168,76,0.06)'
    ctx.beginPath()
    ctx.arc(node.x, node.y, 26, 0, Math.PI * 2)
    ctx.fill()

    ctx.strokeStyle = 'rgba(201,168,76,0.35)'
    ctx.lineWidth   = 1
    ctx.beginPath()
    ctx.arc(node.x, node.y, 26, 0, Math.PI * 2)
    ctx.stroke()

    ctx.strokeStyle = 'rgba(201,168,76,0.9)'
    ctx.lineWidth   = 2
    ctx.beginPath()
    ctx.arc(node.x, node.y, 26, -Math.PI / 2, -Math.PI / 2 + node.val * Math.PI * 2)
    ctx.stroke()

    ctx.fillStyle  = 'rgba(201,168,76,0.95)'
    ctx.font       = '11px "IBM Plex Mono", monospace'
    ctx.textAlign  = 'center'
    ctx.fillText((node.val * 100).toFixed(0) + '%', node.x, node.y + 4)

    ctx.fillStyle = 'rgba(240,240,240,0.38)'
    ctx.font      = '8px "IBM Plex Mono", monospace'
    ctx.fillText(node.label, node.x, node.y + 44)
  })
}

function drawBook(ctx: CanvasRenderingContext2D, t: number, w: number, h: number) {
  ctx.clearRect(0, 0, w, h)

  const cx     = w / 2
  const levels = 5
  const lh     = (h - 58) / levels
  const maxBar = cx - 36

  const bids = Array.from({ length: levels }, (_, i) => ({
    price: 50 - i,
    size:  (levels - i) * 0.17 + Math.sin(t * 0.55 + i * 0.9) * 0.05,
  }))
  const asks = Array.from({ length: levels }, (_, i) => ({
    price: 51 + i,
    size:  (levels - i) * 0.17 + Math.sin(t * 0.55 + i * 0.9 + 1.8) * 0.05,
  }))

  bids.forEach((lv, i) => {
    const bw = Math.max(4, lv.size * maxBar)
    const y  = 30 + i * lh
    const g  = ctx.createLinearGradient(cx - bw, y, cx, y)
    g.addColorStop(0, 'rgba(74,222,128,0.04)')
    g.addColorStop(1, 'rgba(74,222,128,0.38)')
    ctx.fillStyle = g
    ctx.fillRect(cx - bw, y, bw, lh - 4)
    ctx.fillStyle  = 'rgba(240,240,240,0.32)'
    ctx.font       = '9px "IBM Plex Mono", monospace'
    ctx.textAlign  = 'right'
    ctx.fillText(String(lv.price), cx - 6, y + lh / 2 + 3)
  })

  asks.forEach((lv, i) => {
    const bw = Math.max(4, lv.size * maxBar)
    const y  = 30 + i * lh
    const g  = ctx.createLinearGradient(cx, y, cx + bw, y)
    g.addColorStop(0, 'rgba(248,113,113,0.38)')
    g.addColorStop(1, 'rgba(248,113,113,0.04)')
    ctx.fillStyle = g
    ctx.fillRect(cx, y, bw, lh - 4)
    ctx.fillStyle = 'rgba(240,240,240,0.32)'
    ctx.font      = '9px "IBM Plex Mono", monospace'
    ctx.textAlign = 'left'
    ctx.fillText(String(lv.price), cx + 6, y + lh / 2 + 3)
  })

  ctx.strokeStyle = 'rgba(201,168,76,0.28)'
  ctx.lineWidth   = 1
  ctx.setLineDash([3, 5])
  ctx.beginPath()
  ctx.moveTo(cx, 22)
  ctx.lineTo(cx, h - 22)
  ctx.stroke()
  ctx.setLineDash([])

  ctx.font      = '9px "IBM Plex Mono", monospace'
  ctx.textAlign = 'center'
  ctx.fillStyle = 'rgba(74,222,128,0.65)'
  ctx.fillText('BID', cx - 52, 18)
  ctx.fillStyle = 'rgba(248,113,113,0.65)'
  ctx.fillText('ASK', cx + 52, 18)
  ctx.fillStyle = 'rgba(201,168,76,0.55)'
  ctx.fillText('spread: 1', cx, h - 8)
}

// Floor 4: orbiting suit symbols around a pulsing gold ring — CTA
function drawCTA(ctx: CanvasRenderingContext2D, t: number, w: number, h: number) {
  ctx.clearRect(0, 0, w, h)

  const cx     = w / 2
  const cy     = h / 2
  const orbitR = 68
  const suits  = ['♠', '♣', '♥', '♦'] as const

  // Outer glow
  const glow = ctx.createRadialGradient(cx, cy, 0, cx, cy, orbitR + 20)
  glow.addColorStop(0, 'rgba(201,168,76,0.14)')
  glow.addColorStop(1, 'rgba(201,168,76,0)')
  ctx.fillStyle = glow
  ctx.beginPath()
  ctx.arc(cx, cy, orbitR + 20, 0, Math.PI * 2)
  ctx.fill()

  // Dashed orbit ring
  ctx.strokeStyle = 'rgba(201,168,76,0.14)'
  ctx.lineWidth   = 1
  ctx.setLineDash([4, 7])
  ctx.beginPath()
  ctx.arc(cx, cy, orbitR, 0, Math.PI * 2)
  ctx.stroke()
  ctx.setLineDash([])

  // Inner pulsing ring
  const pulse = 0.85 + Math.sin(t * 1.4) * 0.08
  ctx.strokeStyle = `rgba(201,168,76,${(0.55 + Math.sin(t * 1.4) * 0.15).toFixed(2)})`
  ctx.lineWidth   = 1.5
  ctx.beginPath()
  ctx.arc(cx, cy, 22 * pulse, 0, Math.PI * 2)
  ctx.stroke()

  // Core dot
  ctx.fillStyle = 'rgba(201,168,76,0.4)'
  ctx.beginPath()
  ctx.arc(cx, cy, 6 * pulse, 0, Math.PI * 2)
  ctx.fill()

  // Orbiting suit symbols
  suits.forEach((suit, i) => {
    const angle = t * 0.38 + (i * Math.PI / 2)
    const sx    = cx + Math.cos(angle) * orbitR
    const sy    = cy + Math.sin(angle) * orbitR
    const alpha = 0.65 + Math.sin(t * 0.9 + i * 0.8) * 0.2

    ctx.fillStyle  = `rgba(201,168,76,${alpha.toFixed(2)})`
    ctx.font       = '16px serif'
    ctx.textAlign  = 'center'
    ctx.textBaseline = 'middle'
    ctx.fillText(suit, sx, sy)
  })
  ctx.textBaseline = 'alphabetic'
}

// ── Floor definitions ──────────────────────────────────────────────────────

interface Floor {
  id: string
  eyebrow: string
  headline: string
  equation: string
  body: string
  draw: (ctx: CanvasRenderingContext2D, t: number, w: number, h: number) => void
  ctaHref?: string
  ctaLabel?: string
}

const FLOORS: Floor[] = [
  {
    id: 'ev',
    eyebrow: 'Layer 01 — Expected Value',
    headline: 'Price your edge.',
    equation: 'E[V] = Σ pᵢ · vᵢ',
    body: "Every order you place is a wager. The players who win consistently aren't luckier — they're more precise. Expected value transforms gut feeling into a repeatable process.",
    draw: drawEV,
  },
  {
    id: 'bayes',
    eyebrow: 'Layer 02 — Bayesian Inference',
    headline: 'Every trade reveals a hand.',
    equation: 'P(H|E) ∝ P(E|H) · P(H)',
    body: 'The order book is a signal. Which suits does your opponent lift? Which prices do they defend? Each observation narrows your uncertainty about their goal suit.',
    draw: drawBayes,
  },
  {
    id: 'queue',
    eyebrow: 'Layer 03 — Persistent Markets',
    headline: 'When the book persists, so does your alpha.',
    equation: 'priority: price → time → size',
    body: 'In the advanced game, orders survive each trade. Queue position becomes edge. The spread encodes belief. Maker-taker dynamics reward patience and punish impatience.',
    draw: drawBook,
  },
  {
    id: 'cta',
    eyebrow: 'Layer 04 — Enter the Market',
    headline: 'Theory is prologue.',
    equation: 'E[skill] → ∞ as rounds → ∞',
    body: 'The only way to build an edge is to sit at the table. Join a lobby, watch the order flow, and start making markets.',
    draw: drawCTA,
    ctaHref: '/auth',
    ctaLabel: 'Play now',
  },
]

// Camera reaches maxZ at (N-1)/N of scroll progress, giving one full
// floor-length of dwell on the last floor before the section exits.
const FLOOR_DEPTH   = 900
const N             = FLOORS.length
const DWELL_FACTOR  = (N - 1) / N  // 0.75 for 4 floors

export function MathDive({ reducedMotion }: MathDiveProps) {
  const outerRef   = useRef<HTMLElement>(null)
  const worldRef   = useRef<HTMLDivElement>(null)
  const canvasRefs = useRef<(HTMLCanvasElement | null)[]>([])
  const rafRef     = useRef<number>(0)

  useEffect(() => {
    if (reducedMotion) return

    function loop(now: number) {
      const t = now / 1000
      canvasRefs.current.forEach((canvas, i) => {
        if (!canvas) return
        const ctx = canvas.getContext('2d')
        if (!ctx) return
        FLOORS[i].draw(ctx, t, canvas.width, canvas.height)
      })
      rafRef.current = requestAnimationFrame(loop)
    }

    rafRef.current = requestAnimationFrame(loop)
    return () => cancelAnimationFrame(rafRef.current)
  }, [reducedMotion])

  useEffect(() => {
    if (reducedMotion || !outerRef.current || !worldRef.current) return

    let st: { kill(): void } | null = null
    const maxZ = FLOOR_DEPTH * (N - 1)

    import('gsap').then(({ gsap }) => {
      import('gsap/ScrollTrigger').then(({ ScrollTrigger }) => {
        gsap.registerPlugin(ScrollTrigger)
        if (!outerRef.current || !worldRef.current) return

        const world = worldRef.current

        st = ScrollTrigger.create({
          trigger: outerRef.current,
          start: 'top top',
          end: 'bottom bottom',
          scrub: 0.5,
          onUpdate: ({ progress }: { progress: number }) => {
            // Reach maxZ at DWELL_FACTOR, clamp there for the remaining scroll
            const z = Math.min(progress / DWELL_FACTOR, 1) * maxZ
            world.style.transform = `translateZ(${z}px)`
          },
        })
      })
    }).catch(() => {})

    return () => { st?.kill() }
  }, [reducedMotion])

  if (reducedMotion) {
    return (
      <section className="math-dive-outer" aria-label="Mathematical depth">
        <div className="math-dive-static">
          {FLOORS.map(floor => (
            <div key={floor.id} className="math-dive-static-card">
              <span className="md-eyebrow">{floor.eyebrow}</span>
              <h3 className="md-headline">{floor.headline}</h3>
              <p className="md-equation">{floor.equation}</p>
              <p className="md-body">{floor.body}</p>
              {floor.ctaHref && (
                <a href={floor.ctaHref} className="md-cta-btn">{floor.ctaLabel}</a>
              )}
            </div>
          ))}
        </div>
      </section>
    )
  }

  return (
    <section ref={outerRef} className="math-dive-outer" aria-label="Mathematical depth">
      <div className="math-dive-sticky">
        <div className="md-tunnel" aria-hidden="true" />
        <div
          ref={worldRef}
          className="math-dive-world"
          style={{ transform: 'translateZ(0px)' }}
        >
          {FLOORS.map((floor, i) => (
            <div
              key={floor.id}
              className="md-floor"
              style={{ transform: `translateZ(${-i * FLOOR_DEPTH}px)` }}
            >
              <div className="md-panel">
                <div className="md-panel-left">
                  <span className="md-eyebrow">{floor.eyebrow}</span>
                  <h3 className="md-headline">{floor.headline}</h3>
                  <p className="md-equation">{floor.equation}</p>
                  <p className="md-body">{floor.body}</p>
                  {floor.ctaHref && (
                    <a href={floor.ctaHref} className="md-cta-btn">{floor.ctaLabel}</a>
                  )}
                </div>
                <div className="md-panel-right">
                  <canvas
                    ref={el => { canvasRefs.current[i] = el }}
                    className="md-canvas"
                    width={260}
                    height={190}
                  />
                </div>
              </div>
            </div>
          ))}
        </div>
      </div>
    </section>
  )
}
