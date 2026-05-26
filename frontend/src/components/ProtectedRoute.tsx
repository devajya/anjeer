import { Navigate } from 'react-router-dom'
import type { ReactNode } from 'react'
import { useAuth } from '../hooks/useAuth'

interface Props { children: ReactNode }

// AGENT-CTX: Returns null (blank) while loading to prevent a flash redirect
// before the /players/me fetch resolves. An alternative is a loading spinner,
// but null keeps the component free of any design dependency at this stage.
export function ProtectedRoute({ children }: Props) {
  const { user, loading } = useAuth()

  if (loading)     return null
  if (user === null) return <Navigate to="/auth" replace />

  return <>{children}</>
}
