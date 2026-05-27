import { Routes, Route, Navigate, useSearchParams, useLocation } from 'react-router-dom'
import type { ReactNode } from 'react'
import { AnimatePresence, motion } from 'framer-motion'
import { AuthProvider } from './context/AuthContext'
import { ProtectedRoute } from './components/ProtectedRoute'
import { AppNav } from './components/AppNav'
import { Login } from './pages/Login'
import { LandingPage } from './pages/LandingPage'
import { Game } from './pages/Game'
import { LobbyBrowser } from './pages/LobbyBrowser'
import { LobbyRoom } from './pages/LobbyRoom'
import { KeybindSettings } from './pages/KeybindSettings'
import { ApiKeySettings } from './pages/ApiKeySettings'
import { SpectatorView } from './pages/SpectatorView'
import { DocsPage } from './pages/DocsPage'
import { LearnPage } from './pages/LearnPage'

// AGENT-CTX: Prevents direct access to /game without a lobby_id query param.
// A valid lobby_id is required because Game.tsx uses it for WS join and reconnect
// token storage. Without it there is no recoverable game state to present.
function GameRouteGuard({ children }: { children: ReactNode }) {
  const [searchParams] = useSearchParams()
  if (!searchParams.get('lobby_id')) return <Navigate to="/lobby" replace />
  return <>{children}</>
}

// Fade + slight upward slide for all non-game pages.
// Game is excluded: it never unmounts mid-session and overlay components
// (RoundEndModal, InterRoundScreen) handle their own entrance animations.
function PageTransition({ children }: { children: ReactNode }) {
  return (
    <motion.div
      initial={{ opacity: 0, y: 8 }}
      animate={{ opacity: 1, y: 0 }}
      exit={{ opacity: 0, y: -4 }}
      transition={{ duration: 0.2, ease: 'easeOut' }}
    >
      {children}
    </motion.div>
  )
}

// Paths that show AppNav. Checked as prefix so /lobby/:code also matches.
const NAV_PREFIXES = ['/lobby', '/docs', '/api-keys', '/settings', '/learn']

// AGENT-CTX: AppNav sits outside AnimatePresence so it stays mounted and
// doesn't flash during route transitions. The content wrapper shifts down
// 48px with app-content--with-nav when the nav is visible.
// AnimatePresence key=pathname means each unique path gets its own animation cycle.
function AnimatedRoutes() {
  const location = useLocation()
  const showNav  = NAV_PREFIXES.some(p =>
    location.pathname === p || location.pathname.startsWith(p + '/'),
  )
  return (
    <>
      {showNav && <AppNav />}
      <div className={showNav ? 'app-content app-content--with-nav' : 'app-content'}>
        <AnimatePresence mode="wait">
          <Routes location={location} key={location.pathname}>
        <Route path="/" element={<PageTransition><LandingPage /></PageTransition>} />
        <Route path="/auth" element={<PageTransition><Login /></PageTransition>} />
        <Route
          path="/lobby"
          element={
            <ProtectedRoute>
              <PageTransition><LobbyBrowser /></PageTransition>
            </ProtectedRoute>
          }
        />
        <Route
          path="/lobby/:code"
          element={
            <ProtectedRoute>
              <PageTransition><LobbyRoom /></PageTransition>
            </ProtectedRoute>
          }
        />
        {/* Game: no PageTransition — full real estate, game feel must not regress */}
        <Route
          path="/game"
          element={
            <ProtectedRoute>
              <GameRouteGuard>
                <Game />
              </GameRouteGuard>
            </ProtectedRoute>
          }
        />
        <Route
          path="/settings/keybinds"
          element={
            <ProtectedRoute>
              <PageTransition><KeybindSettings /></PageTransition>
            </ProtectedRoute>
          }
        />
        <Route
          path="/api-keys"
          element={
            <ProtectedRoute>
              <PageTransition><ApiKeySettings /></PageTransition>
            </ProtectedRoute>
          }
        />
        <Route
          path="/spectate/:lobbyCode"
          element={
            <ProtectedRoute>
              <PageTransition><SpectatorView /></PageTransition>
            </ProtectedRoute>
          }
        />
        <Route
          path="/docs"
          element={
            <ProtectedRoute>
              <PageTransition><DocsPage /></PageTransition>
            </ProtectedRoute>
          }
        />
        <Route
          path="/learn"
          element={
            <ProtectedRoute>
              <PageTransition><LearnPage /></PageTransition>
            </ProtectedRoute>
          }
        />
        {/* AGENT-CTX: Catch-all redirects to / (LandingPage) — unauthenticated
            users see the landing page, then navigate to /auth to sign in.
            Changed from /lobby in Slice 12 now that the landing page is the
            public entry point. ProtectedRoute still redirects to /auth for
            any authenticated-only path accessed without a session. */}
        <Route path="*" element={<Navigate to="/" replace />} />
          </Routes>
        </AnimatePresence>
      </div>
    </>
  )
}

export default function App() {
  return (
    <AuthProvider>
      <AnimatedRoutes />
    </AuthProvider>
  )
}
