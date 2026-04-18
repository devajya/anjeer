import { createContext, useState, useEffect, useCallback } from 'react'
import type { ReactNode } from 'react'
import type { AuthUser, AuthContextValue } from '../types/auth'

// AGENT-CTX: Default context value is used only if a component consumes AuthContext
// outside of an AuthProvider. The loading:true default prevents ProtectedRoute from
// flashing a redirect before the real auth state arrives. Do not change to false.
export const AuthContext = createContext<AuthContextValue>({
  user:    null,
  loading: true,
  logout:  async () => {},
})

export function AuthProvider({ children }: { children: ReactNode }) {
  const [user, setUser]       = useState<AuthUser | null>(null)
  const [loading, setLoading] = useState(true)

  useEffect(() => {
    // credentials:'include' sends the httpOnly access_token cookie cross-origin
    // (needed in production where the frontend and API are on different origins).
    async function init() {
      try {
        let res = await fetch('/players/me', { credentials: 'include' })
        if (!res.ok) {
          const body: { error?: string } = await res.json().catch(() => ({}))
          if (body.error === 'TOKEN_EXPIRED') {
            // Access token stale but refresh token may be valid — try silently.
            const refresh = await fetch('/auth/refresh', { method: 'POST', credentials: 'include' })
            if (refresh.ok) {
              res = await fetch('/players/me', { credentials: 'include' })
            }
          }
        }
        setUser(res.ok ? (await res.json() as AuthUser) : null)
      } catch {
        setUser(null)
      } finally {
        setLoading(false)
      }
    }
    init()
  }, [])

  // AGENT-CTX: logout does NOT navigate to /login here. After setUser(null),
  // ProtectedRoute re-renders, sees user===null, and issues <Navigate to="/login">.
  // Keeping navigation out of AuthService avoids router coupling in this context.
  const logout = useCallback(async () => {
    await fetch('/auth/logout', { method: 'POST', credentials: 'include' })
    setUser(null)
  }, [])

  return (
    <AuthContext.Provider value={{ user, loading, logout }}>
      {children}
    </AuthContext.Provider>
  )
}
