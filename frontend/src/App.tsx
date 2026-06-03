import { lazy, Suspense } from 'react'
import { Routes, Route, Navigate, useSearchParams, useLocation } from 'react-router-dom'
import type { ReactNode } from 'react'
import { AnimatePresence, motion } from 'framer-motion'
import { AuthProvider } from './context/AuthContext'
import { ProtectedRoute } from './components/ProtectedRoute'
import { AppNav } from './components/AppNav'

// Route-level code splitting: each page is its own chunk, loaded on demand.
// ProtectedRoute and AppNav stay static — they are tiny and needed on every route.
const LandingPage      = lazy(() => import('./pages/LandingPage').then(m => ({ default: m.LandingPage })))
const Login            = lazy(() => import('./pages/Login').then(m => ({ default: m.Login })))
const Game             = lazy(() => import('./pages/Game').then(m => ({ default: m.Game })))
const LobbyBrowser     = lazy(() => import('./pages/LobbyBrowser').then(m => ({ default: m.LobbyBrowser })))
const LobbyRoom        = lazy(() => import('./pages/LobbyRoom').then(m => ({ default: m.LobbyRoom })))
const KeybindSettings  = lazy(() => import('./pages/KeybindSettings').then(m => ({ default: m.KeybindSettings })))
const SettingsPage     = lazy(() => import('./pages/SettingsPage').then(m => ({ default: m.SettingsPage })))
const ApiKeySettings   = lazy(() => import('./pages/ApiKeySettings').then(m => ({ default: m.ApiKeySettings })))
const SpectatorView    = lazy(() => import('./pages/SpectatorView').then(m => ({ default: m.SpectatorView })))
const DocsPage         = lazy(() => import('./pages/DocsPage').then(m => ({ default: m.DocsPage })))
const LearnPage        = lazy(() => import('./pages/LearnPage').then(m => ({ default: m.LearnPage })))

// AGENT-CTX: Prevents direct access to /game without a lobby_id query param.
function GameRouteGuard({ children }: { children: ReactNode }) {
  const [searchParams] = useSearchParams()
  if (!searchParams.get('lobby_id')) return <Navigate to="/lobby" replace />
  return <>{children}</>
}

// Fade + slight upward slide for all non-game pages.
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

const NAV_PREFIXES = ['/lobby', '/docs', '/api-keys', '/settings', '/learn']

// AGENT-CTX: AppNav sits outside AnimatePresence so it stays mounted and
// doesn't flash during route transitions. Suspense wraps AnimatePresence so
// lazy-chunk loading doesn't interfere with exit-animation tracking —
// AnimatePresence sees Routes directly and tracks key changes correctly.
function AnimatedRoutes() {
  const location = useLocation()
  const showNav  = NAV_PREFIXES.some(p =>
    location.pathname === p || location.pathname.startsWith(p + '/'),
  )
  return (
    <>
      {showNav && <AppNav />}
      <div className={showNav ? 'app-content app-content--with-nav' : 'app-content'}>
        <Suspense fallback={null}>
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
                path="/settings"
                element={<Navigate to="/settings/keybinds" replace />}
              />
              <Route
                path="/settings/keybinds"
                element={
                  <ProtectedRoute>
                    <PageTransition><SettingsPage /></PageTransition>
                  </ProtectedRoute>
                }
              />
              <Route
                path="/settings/preferences"
                element={
                  <ProtectedRoute>
                    <PageTransition><SettingsPage /></PageTransition>
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
              {/* AGENT-CTX: Catch-all redirects to / — unauthenticated users see
                  the landing page, then navigate to /auth to sign in. */}
              <Route path="*" element={<Navigate to="/" replace />} />
            </Routes>
          </AnimatePresence>
        </Suspense>
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
