import { Routes, Route, Navigate, useSearchParams } from 'react-router-dom'
import type { ReactNode } from 'react'
import { AuthProvider } from './context/AuthContext'
import { ProtectedRoute } from './components/ProtectedRoute'
import { Login } from './pages/Login'
import { Game } from './pages/Game'
import { LobbyBrowser } from './pages/LobbyBrowser'
import { LobbyRoom } from './pages/LobbyRoom'
import { KeybindSettings } from './pages/KeybindSettings'
import { ApiKeySettings } from './pages/ApiKeySettings'
import { SpectatorView } from './pages/SpectatorView'
import { DocsPage } from './pages/DocsPage'

// AGENT-CTX: Prevents direct access to /game without a lobby_id query param.
// A valid lobby_id is required because Game.tsx uses it for WS join and reconnect
// token storage. Without it there is no recoverable game state to present.
function GameRouteGuard({ children }: { children: ReactNode }) {
  const [searchParams] = useSearchParams()
  if (!searchParams.get('lobby_id')) return <Navigate to="/lobby" replace />
  return <>{children}</>
}

// AGENT-CTX: App.tsx is the route map. Lobby pages sit between login and game.
// Default route redirects to /lobby so new users land in the lobby browser.
// /game is only reached via lobby_started navigation (LobbyRoom → /game?lobby_id=...).
// BrowserRouter is in main.tsx, not here, so this tree is testable without a router.
export default function App() {
  return (
    <AuthProvider>
      <Routes>
        <Route path="/login" element={<Login />} />
        <Route
          path="/lobby"
          element={
            <ProtectedRoute>
              <LobbyBrowser />
            </ProtectedRoute>
          }
        />
        <Route
          path="/lobby/:code"
          element={
            <ProtectedRoute>
              <LobbyRoom />
            </ProtectedRoute>
          }
        />
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
              <KeybindSettings />
            </ProtectedRoute>
          }
        />
        <Route
          path="/api-keys"
          element={
            <ProtectedRoute>
              <ApiKeySettings />
            </ProtectedRoute>
          }
        />
        <Route
          path="/spectate/:lobbyCode"
          element={
            <ProtectedRoute>
              <SpectatorView />
            </ProtectedRoute>
          }
        />
        <Route
          path="/docs"
          element={
            <ProtectedRoute>
              <DocsPage />
            </ProtectedRoute>
          }
        />
        {/* AGENT-CTX: Catch-all redirects to /lobby so unauthenticated users
            hit the lobby browser (ProtectedRoute will then send them to /login).
            Previously /* rendered Game directly — changed in Slice 6 now that
            the lobby is the entry point. */}
        <Route path="*" element={<Navigate to="/lobby" replace />} />
      </Routes>
    </AuthProvider>
  )
}
