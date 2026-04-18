import { useContext } from 'react'
import { AuthContext } from '../context/AuthContext'
import type { AuthContextValue } from '../types/auth'

export function useAuth(): AuthContextValue {
  return useContext(AuthContext)
}
