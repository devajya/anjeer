import type { LobbyView, ListLobbiesResponse } from '../types/lobby'

export async function listLobbies(): Promise<LobbyView[]> {
  const res = await fetch('/lobbies', { credentials: 'include' })
  if (!res.ok) throw new Error('Failed to load lobbies')
  const data: ListLobbiesResponse = await res.json()
  return data.lobbies
}

export async function findLobbyByCode(code: string | undefined): Promise<LobbyView | undefined> {
  if (!code) return undefined
  const lobbies = await listLobbies()
  return lobbies.find(l => l.code === code)
}
