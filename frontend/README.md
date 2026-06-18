# Frontend

React + TypeScript + Vite. State flows down from hooks; components are stateless consumers of props.

## Architecture

The WebSocket connection is managed by a single `useWebSocket` hook that owns all game state and dispatches incoming messages by type. Keeping the connection and state in one place means components never have to coordinate with each other about what the server said — they just read props.

Route-level code splitting via `React.lazy()` keeps the initial bundle small. The landing page's below-fold sections are additionally deferred behind a `Suspense` boundary since they carry the heaviest animation dependencies (GSAP, Framer Motion).

## Market Data Views

The game UI supports three view modes driven by the lobby's game mode:

- **Simple** — best bid/ask per suit only
- **Intermediate** — full MBP-N price-level depth ladder; multi-quantity order inputs
- **Advanced** — MBO event log showing individual order lifecycle events (added, executed, cancelled) with sequence numbers; partial fills; no book wipe after trades

The view mode is set at lobby creation and received in `round_start` — components read it from `GameConfigContext` rather than each fetching it independently.

## Landing Page

The landing page is built with GSAP ScrollTrigger and Framer Motion. Key sections:

- **Hero** — scroll-pinned expanding portal with mouse parallax
- **Rules accordion** — auto-advancing step-by-step game rules with a canvas chip animation
- **Slideshow** — 400vh sticky with an SVG stepped-border frame and GSAP pill indicator
- **Math dive** — CSS 3D camera move through EV → Bayes → order book, ending at the CTA

The landing page is public (no auth required) and is entirely separate from the game UI routes.

## Design

All colors, typography, spacing, and radii are defined as CSS custom properties in `src/styles/tokens.css`. No magic numbers in component styles. This makes global visual changes — dark mode adjustments, spacing tweaks — a one-file edit.

The wire protocol's discriminated union types live in `src/types/messages.ts` and are the single source of truth for what the server can send. Any protocol change starts there.
