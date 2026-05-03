import { Routes, Route, Navigate } from 'react-router-dom'
import { AuthProvider } from './context/AuthContext'
import { ProtectedRoute } from './components/ProtectedRoute'
import { Login } from './pages/Login'
import { Game } from './pages/Game'
import { LobbyBrowser } from './pages/LobbyBrowser'
import { LobbyRoom } from './pages/LobbyRoom'
import { KeybindSettings } from './pages/KeybindSettings'

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
              <Game />
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
        {/* AGENT-CTX: Catch-all redirects to /lobby so unauthenticated users
            hit the lobby browser (ProtectedRoute will then send them to /login).
            Previously /* rendered Game directly — changed in Slice 6 now that
            the lobby is the entry point. */}
        <Route path="*" element={<Navigate to="/lobby" replace />} />
      </Routes>
    </AuthProvider>
  )
}
