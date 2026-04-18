// AuthUser mirrors the GET /players/me response shape exactly.
// games_played is stubbed at 0 until Slice 7 wires game session completion.
export interface AuthUser {
  id:           number
  username:     string
  games_played: number
}

export interface AuthContextValue {
  user:    AuthUser | null
  loading: boolean
  logout:  () => Promise<void>
}
