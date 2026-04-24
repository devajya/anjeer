// AGENT-CTX: REST API shapes for the lobby HTTP endpoints (port 10000).
// Separate from messages.ts which mirrors the WebSocket wire protocol.
// The LobbyView shape must match the JSON returned by GET /lobbies and POST /lobbies.

export interface LobbyView {
  id:          string
  code:        string
  owner_id:    number
  status:      'waiting' | 'starting' | 'in_game' | 'finished' | 'closed'
  min_players: number
  max_players: number
  player_count: number
  created_at:  string
}

export interface ListLobbiesResponse {
  lobbies: LobbyView[]
}
