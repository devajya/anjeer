import { useState, useEffect, useRef, useCallback } from 'react'
import type { ClientCommand } from '../types/messages'

export type ReconnectStatus =
  | 'connected'
  | 'disconnected'
  | 'reconnecting'
  | 'reattached'
  | 'expired'

interface StoredToken {
  token: string
  expires_at: number  // Unix epoch ms
}

function storageKey(lobbyId: string): string {
  return `anjeer_reconnect_${lobbyId}`
}


/**
 * Manages the client-side reconnect state machine for a game slot.
 *
 * AGENT-CTX: This hook does NOT own game state (hand, books, etc.) — that lives
 * in useWebSocket. It only owns: localStorage token persistence, the
 * connected→reconnecting→reattached|expired status machine, and the countdown.
 *
 * Callers (Game.tsx) must wire:
 *   onTokenReceived   → called when useWebSocket surfaces a reconnect_token message
 *   onSnapshotReceived → called when useWebSocket surfaces a game_state_snapshot
 *   onWindowExpired   → called when useWebSocket surfaces reconnect_window_expired
 *
 * The hook detects WS drops via the `connected` prop transition (false→reconnecting,
 * true→send reconnect_game if token present). This avoids coupling to the WS
 * internals — Game.tsx already reads `connected` from useWebSocket.
 */
export function useReconnect(
  lobbyId: string,
  windowSeconds: number,
  connected: boolean,
  sendMessage: (cmd: ClientCommand) => void,
): {
  status: ReconnectStatus
  remainingSeconds: number
  initiateReconnect: () => void
  onTokenReceived:   (token: string, expiresAt: number) => void
  onSnapshotReceived: () => void
  onWindowExpired:   () => void
} {
  const [status, setStatus]           = useState<ReconnectStatus>('connected')
  const [remainingSeconds, setRemaining] = useState(windowSeconds)

  // Refs so effects can read current values without being in their dep arrays.
  const storedTokenRef  = useRef<string | null>(null)
  const statusRef       = useRef<ReconnectStatus>('connected')
  const prevConnectedRef = useRef(connected)

  useEffect(() => { statusRef.current = status }, [status])

  // On mount: restore token from localStorage and attempt immediate reconnect.
  // AGENT-CTX: handles "direct game URL with stored token in new tab" scenario.
  // We bypass readStoredToken here so we can distinguish "no token" (silent) from
  // "token exists but expired" (show ReconnectOverlay immediately).
  useEffect(() => {
    const raw = localStorage.getItem(storageKey(lobbyId))
    if (!raw) return
    let parsed: StoredToken
    try { parsed = JSON.parse(raw) as StoredToken }
    catch { localStorage.removeItem(storageKey(lobbyId)); return }

    if (parsed.expires_at < Date.now()) {
      localStorage.removeItem(storageKey(lobbyId))
      setStatus('expired')
      return
    }

    storedTokenRef.current = parsed.token
    const remaining = Math.max(1, Math.floor((parsed.expires_at - Date.now()) / 1000))
    setRemaining(remaining)
    setStatus('reconnecting')
    if (connected) {
      sendMessage({ type: 'reconnect_game', lobby_id: lobbyId, token: parsed.token })
    }
  }, []) // intentional: mount-only read of localStorage

  // Detect WS connect/disconnect transitions.
  useEffect(() => {
    const wasConnected = prevConnectedRef.current
    prevConnectedRef.current = connected

    if (!connected && wasConnected && storedTokenRef.current) {
      // WS dropped while a token is held — enter reconnecting.
      setStatus(prev => (prev === 'expired' ? prev : 'reconnecting'))
    } else if (connected && !wasConnected && storedTokenRef.current
               && statusRef.current === 'reconnecting') {
      // WS recovered — send reattach command.
      sendMessage({ type: 'reconnect_game', lobby_id: lobbyId, token: storedTokenRef.current })
    }
  }, [connected, lobbyId, sendMessage])

  // Countdown timer while reconnecting.
  // AGENT-CTX: effect depends only on `status` (not remainingSeconds) so a single
  // interval is started per reconnecting entry. setRemaining functional-update form
  // reads fresh state without the effect needing remainingSeconds in deps.
  useEffect(() => {
    if (status !== 'reconnecting') return
    const id = setInterval(() => {
      setRemaining(prev => {
        const next = prev - 1
        if (next <= 0) {
          setStatus('expired')
          localStorage.removeItem(storageKey(lobbyId))
          storedTokenRef.current = null
          return 0
        }
        return next
      })
    }, 1000)
    return () => clearInterval(id)
  }, [status, lobbyId])

  // ── Public callbacks ────────────────────────────────────────────────────

  const onTokenReceived = useCallback((token: string, expiresAt: number) => {
    storedTokenRef.current = token
    localStorage.setItem(storageKey(lobbyId), JSON.stringify({ token, expires_at: expiresAt }))
    setStatus('connected')
    setRemaining(windowSeconds)
  }, [lobbyId, windowSeconds])

  const onSnapshotReceived = useCallback(() => {
    setStatus('reattached')
  }, [])

  const onWindowExpired = useCallback(() => {
    setStatus('expired')
    localStorage.removeItem(storageKey(lobbyId))
    storedTokenRef.current = null
  }, [lobbyId])

  const initiateReconnect = useCallback(() => {
    if (!storedTokenRef.current || !connected) return
    sendMessage({ type: 'reconnect_game', lobby_id: lobbyId, token: storedTokenRef.current })
    setStatus('reconnecting')
    setRemaining(windowSeconds)
  }, [lobbyId, windowSeconds, connected, sendMessage])

  return {
    status,
    remainingSeconds,
    initiateReconnect,
    onTokenReceived,
    onSnapshotReceived,
    onWindowExpired,
  }
}
