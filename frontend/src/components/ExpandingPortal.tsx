import { useEffect, useRef, useState } from 'react'
import { motion } from 'framer-motion'
import { useMouseParallax } from '../hooks/useMouseParallax'
import './ExpandingPortal.css'

export interface ExpandingPortalProps {
  reducedMotion: boolean
  isMobile: boolean
}

const HERO_CONTENT = {
  eyebrow: 'Multiplayer card trading',
  headline: 'Anjeer',
  body: 'Four players. Four suits. One hidden goal. Trade your way to a majority before time runs out.',
}

function CornerBrackets() {
  return (
    <>
      <svg className="ep-corner ep-corner--tl" width="12" height="12" viewBox="0 0 12 12" fill="none" aria-hidden="true">
        <path d="M12 0H0V12" stroke="currentColor" strokeWidth="1.5" />
      </svg>
      <svg className="ep-corner ep-corner--tr" width="12" height="12" viewBox="0 0 12 12" fill="none" aria-hidden="true">
        <path d="M0 0H12V12" stroke="currentColor" strokeWidth="1.5" />
      </svg>
      <svg className="ep-corner ep-corner--bl" width="12" height="12" viewBox="0 0 12 12" fill="none" aria-hidden="true">
        <path d="M12 12H0V0" stroke="currentColor" strokeWidth="1.5" />
      </svg>
      <svg className="ep-corner ep-corner--br" width="12" height="12" viewBox="0 0 12 12" fill="none" aria-hidden="true">
        <path d="M0 12H12V0" stroke="currentColor" strokeWidth="1.5" />
      </svg>
    </>
  )
}

export function ExpandingPortal({ reducedMotion, isMobile }: ExpandingPortalProps) {
  const startExpanded = reducedMotion || isMobile
  const [isExpanded, setIsExpanded] = useState(startExpanded)
  const sectionRef = useRef<HTMLDivElement>(null)
  // containerRef is on the tilt wrapper — parallax uses it for bounds measurement
  const containerRef = useRef<HTMLDivElement>(null)
  const dotCanvasRef = useRef<HTMLCanvasElement>(null)
  const dotMouseRef = useRef({ x: -9999, y: -9999 })

  const applyParallax = isExpanded && !reducedMotion && !isMobile
  const { normX, normY } = useMouseParallax(
    containerRef as React.RefObject<HTMLElement>,
    applyParallax,
  )

  useEffect(() => {
    if (!isExpanded) return
    const canvas = dotCanvasRef.current
    if (!canvas) return
    const ctx = canvas.getContext('2d')
    if (!ctx) return

    const SPACING = 40
    const DOT_R = 1
    const BASE_A = 0.22
    const GLOW_A = 0.9
    const GLOW_R = 90

    const resize = () => {
      canvas.width = canvas.offsetWidth
      canvas.height = canvas.offsetHeight
    }
    resize()

    const onMouseMove = (e: MouseEvent) => {
      const rect = canvas.getBoundingClientRect()
      dotMouseRef.current = { x: e.clientX - rect.left, y: e.clientY - rect.top }
    }

    const animated = applyParallax
    if (animated) window.addEventListener('mousemove', onMouseMove)

    let rafId: number
    const draw = () => {
      const { width: w, height: h } = canvas
      ctx.clearRect(0, 0, w, h)
      const { x: mx, y: my } = dotMouseRef.current
      for (let gx = SPACING / 2; gx < w + SPACING; gx += SPACING) {
        for (let gy = SPACING / 2; gy < h + SPACING; gy += SPACING) {
          const t = animated ? Math.max(0, 1 - Math.hypot(gx - mx, gy - my) / GLOW_R) : 0
          const r = Math.round(255 + (201 - 255) * t)
          const g = Math.round(255 + (168 - 255) * t)
          const b = Math.round(255 + (76  - 255) * t)
          const a = BASE_A + t * (GLOW_A - BASE_A)
          ctx.fillStyle = `rgba(${r},${g},${b},${a})`
          ctx.beginPath()
          ctx.arc(gx, gy, DOT_R, 0, Math.PI * 2)
          ctx.fill()
        }
      }
      if (animated) rafId = requestAnimationFrame(draw)
    }
    draw()

    const ro = new ResizeObserver(() => { resize(); if (!animated) draw() })
    ro.observe(canvas)

    return () => {
      if (animated) { cancelAnimationFrame(rafId); window.removeEventListener('mousemove', onMouseMove) }
      ro.disconnect()
    }
  }, [isExpanded, applyParallax])

  // AGENT-CTX: ScrollTrigger fires once when the sticky wrapper hits viewport top.
  // onLeaveBack collapses on reverse scroll so the intro replays on scroll-up.
  useEffect(() => {
    if (startExpanded) return

    let st: { kill(): void } | null = null

    import('gsap').then(({ gsap }) => {
      import('gsap/ScrollTrigger').then(({ ScrollTrigger }) => {
        gsap.registerPlugin(ScrollTrigger)
        if (!sectionRef.current) return
        st = ScrollTrigger.create({
          trigger: sectionRef.current,
          start: 'top top',
          onEnter: () => setIsExpanded(true),
          onLeaveBack: () => setIsExpanded(false),
        })
      })
    }).catch(() => {})

    return () => { st?.kill() }
  }, [startExpanded])

  // 3D tilt: cursor position drives rotateX/rotateY so the side closest to the
  // cursor appears to press into the screen while the opposite side lifts toward
  // the viewer. rotateX(-normY*8): top presses in when cursor is at top.
  // rotateY(-normX*8): right presses in when cursor is at right.
  const tiltStyle = applyParallax
    ? {
        transform: `perspective(1000px) rotateX(${-normY * 8}deg) rotateY(${-normX * 8}deg)`,
      }
    : undefined

  return (
    <div
      ref={sectionRef}
      className={`ep-section${startExpanded ? ' ep-section--static' : ' ep-section--scroll'}`}
    >
      <div className="ep-sticky">
        <motion.div
          layout={!reducedMotion}
          data-testid="ep-card"
          data-expanded={isExpanded ? 'true' : 'false'}
          className={`ep-card${isExpanded ? ' ep-card--expanded' : ' ep-card--collapsed'}`}
          transition={reducedMotion ? { duration: 0 } : { type: 'spring', stiffness: 60, damping: 22 }}
        >
          {/* Tilt wrapper: owns 3D perspective rotation. Separate from motion.div
              so framer-motion's layout transform and the 3D tilt don't conflict. */}
          <div ref={containerRef} className="ep-tilt" style={tiltStyle}>
            <CornerBrackets />

            {!isExpanded && (
              <>
                <div className="ep-suits-preview" aria-hidden="true">
                  <span className="ep-suit-preview-symbol ep-suit--spade">♠</span>
                  <span className="ep-suit-preview-symbol ep-suit--club">♣</span>
                  <span className="ep-suit-preview-symbol ep-suit--heart">♥</span>
                  <span className="ep-suit-preview-symbol ep-suit--diamond">♦</span>
                </div>
                <div className="ep-scroll-hint" aria-hidden="true">
                  <span className="ep-scroll-label">scroll to explore</span>
                  <svg className="ep-scroll-chevron" width="14" height="8" viewBox="0 0 14 8" fill="none">
                    <path d="M1 1L7 7L13 1" stroke="currentColor" strokeWidth="1.5" strokeLinecap="round" strokeLinejoin="round"/>
                  </svg>
                </div>
              </>
            )}

            {isExpanded && (
              <>
                {/* Dot canvas: individual dots colored by cursor proximity */}
                <canvas
                  ref={dotCanvasRef}
                  className={`ep-dot-canvas${applyParallax ? ' ep-dot-canvas--fade' : ''}`}
                  style={applyParallax ? { transform: `translate(${normX * 6}px, ${normY * 6}px)` } : undefined}
                />

                {/* Layer 1: large suit symbols */}
                <div
                  className={`ep-layer ep-layer--1${applyParallax ? ' ep-layer--fade' : ''}`}
                  data-intensity="18"
                  style={applyParallax ? { transform: `translate(${normX * 18}px, ${normY * 18}px)` } : undefined}
                >
                  <span className="ep-suit-large ep-suit-large--0" aria-hidden="true">♠</span>
                  <span className="ep-suit-large ep-suit-large--1" aria-hidden="true">♦</span>
                  <span className="ep-suit-large ep-suit-large--2" aria-hidden="true">♥</span>
                  <span className="ep-suit-large ep-suit-large--3" aria-hidden="true">♣</span>
                </div>

                {/* Layer 2: eyebrow + headline + body + CTA */}
                <div
                  className={`ep-layer ep-layer--2${applyParallax ? ' ep-layer--fade' : ''}`}
                  data-intensity="32"
                  style={applyParallax ? { transform: `translate(${normX * 32}px, ${normY * 32}px)` } : undefined}
                >
                  <span className="ep-eyebrow">{HERO_CONTENT.eyebrow}</span>
                  <h1 className="ep-headline">{HERO_CONTENT.headline}</h1>
                  <p className="ep-body">{HERO_CONTENT.body}</p>
                  <a className="ep-cta" href="/auth">Play Now →</a>
                </div>
              </>
            )}
          </div>
        </motion.div>
      </div>
    </div>
  )
}
