import { Routes, Route } from 'react-router-dom'
import { AuthProvider } from './context/AuthContext'
import { ProtectedRoute } from './components/ProtectedRoute'
import { Login } from './pages/Login'
import { Game } from './pages/Game'

// AGENT-CTX: App.tsx is now purely a route map. All game UI lives in pages/Game.tsx.
// AuthProvider wraps everything so any route can access auth state.
// BrowserRouter is in main.tsx, not here, so this tree is testable without a router.
// Slice 6 adds a /lobby route and a /lobby/:id route between /login and /game.
export default function App() {
  return (
    <AuthProvider>
      <Routes>
        <Route path="/login" element={<Login />} />
        <Route
          path="/*"
          element={
            <ProtectedRoute>
              <Game />
            </ProtectedRoute>
          }
        />
      </Routes>
    </AuthProvider>
  )
}
