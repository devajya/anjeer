// Lightweight WS hook for lobby queue management — separate from useWebSocket
// so LobbyBrowser doesn't pull in the full game-session state machine.
import { useState, useRef, useEffect } from 'react'

export type PlayersAround = { position: number; username: string; is_self: boolean }

export type QueueSocketState =
  | { status: 'idle' }
  | { status: 'queued'; lobbyId: string; position: number; queueSize: number; playersAround: PlayersAround[] }
  | { status: 'overflow'; lobbyId: string }
  | { status: 'admitted'; lobbyId: string; slotIndex: number }

export interface UseQueueSocketReturn {
  state: QueueSocketState
  joinQueue: (lobbyId: string) => void
  leaveQueue: () => void
  reset: () => void
}

export function useQueueSocket(): UseQueueSocketReturn {
  const [state, setState] = useState<QueueSocketState>({ status: 'idle' })
  const wsRef      = useRef<WebSocket | null>(null)
  const lobbyIdRef = useRef<string | null>(null)

  function disconnect() {
    if (wsRef.current) {
      wsRef.current.onmessage = null
      wsRef.current.onclose   = null
      wsRef.current.onerror   = null
      wsRef.current.close()
      wsRef.current = null
    }
  }

  function joinQueue(lobbyId: string) {
    disconnect()
    lobbyIdRef.current = lobbyId

    const proto = window.location.protocol === 'https:' ? 'wss:' : 'ws:'
    const ws    = new WebSocket(`${proto}//${window.location.host}/ws`)
    wsRef.current = ws

    ws.onopen = () => {
      ws.send(JSON.stringify({ type: 'join_queue', lobby_id: lobbyId }))
    }

    ws.onmessage = (event: MessageEvent) => {
      try {
        const msg = JSON.parse(event.data as string) as { type: string; [k: string]: unknown }
        switch (msg.type) {
          case 'queue_joined':
            setState({
              status: 'queued',
              lobbyId,
              position:     msg.position    as number,
              queueSize:    msg.queue_size  as number,
              playersAround: [],
            })
            break
          case 'queue_position_update':
            setState({
              status: 'queued',
              lobbyId,
              position:      msg.position       as number,
              queueSize:     msg.queue_size      as number,
              playersAround: (msg.players_around as PlayersAround[]) ?? [],
            })
            break
          case 'queue_left':
            disconnect()
            setState({ status: 'idle' })
            break
          case 'queue_admitted':
            setState({ status: 'admitted', lobbyId, slotIndex: msg.slot_index as number })
            disconnect()
            break
          case 'queue_overflow':
            setState({ status: 'overflow', lobbyId })
            disconnect()
            break
        }
      } catch {
        // ignore malformed frames
      }
    }

    ws.onclose = () => {
      // Only fall back to idle on unexpected close — terminal states stay put.
      setState(prev => prev.status === 'queued' ? { status: 'idle' } : prev)
    }

    ws.onerror = () => {
      disconnect()
      setState({ status: 'idle' })
    }
  }

  function leaveQueue() {
    if (wsRef.current?.readyState === WebSocket.OPEN && lobbyIdRef.current) {
      wsRef.current.send(JSON.stringify({ type: 'leave_queue', lobby_id: lobbyIdRef.current }))
    }
    disconnect()
    setState({ status: 'idle' })
  }

  function reset() {
    disconnect()
    setState({ status: 'idle' })
  }

  useEffect(() => () => disconnect(), [])

  return { state, joinQueue, leaveQueue, reset }
}
