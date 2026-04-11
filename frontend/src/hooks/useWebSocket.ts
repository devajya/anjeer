import { useState, useEffect } from 'react'
import type { ServerMessage } from '../types/messages'

export interface WsState {
  connected: boolean
  /** Last server_ts value received via heartbeat, or null if no heartbeat yet. */
  lastServerTs: number | null
}

// AGENT-CTX: url must be a path starting with '/' (e.g. '/ws').
// The hook constructs an absolute ws:// or wss:// URL from window.location so the
// same code works in dev (proxied by Vite) and production (proxied by nginx).
// Do NOT pass a hardcoded absolute URL — that would break the proxy abstraction.
// See vite.config.ts proxy config and contexts/machine-setup.md for the nginx note.
export function useWebSocket(url: string): WsState {
  const [state, setState] = useState<WsState>({
    connected: false,
    lastServerTs: null,
  })

  useEffect(() => {
    // Derive the correct WS protocol from the page protocol.
    const protocol = window.location.protocol === 'https:' ? 'wss:' : 'ws:'
    const fullUrl = `${protocol}//${window.location.host}${url}`

    const ws = new WebSocket(fullUrl)

    ws.onopen = () => {
      setState(s => ({ ...s, connected: true }))
    }

    // AGENT-CTX: Both onclose and onerror set connected=false.
    // onclose fires when the server closes the connection (including ping timeout).
    // onerror fires on network-level failures; a close event normally follows,
    // but we handle both for safety.
    ws.onclose = () => {
      setState(s => ({ ...s, connected: false }))
    }

    ws.onerror = () => {
      setState(s => ({ ...s, connected: false }))
    }

    ws.onmessage = (event: MessageEvent) => {
      let msg: ServerMessage
      try {
        msg = JSON.parse(event.data as string) as ServerMessage
      } catch {
        // Silently drop malformed messages — log in a future observability slice.
        return
      }

      // AGENT-CTX: Exhaustive switch on msg.type. TypeScript will flag unhandled
      // variants when new message types are added to ServerMessage union.
      switch (msg.type) {
        case 'heartbeat':
          setState(s => ({ ...s, lastServerTs: msg.server_ts }))
          break
        default:
          // AGENT-CTX: Unknown message types are intentionally ignored.
          // Future slices add new cases here as new server events are introduced.
          // Note: TypeScript exhaustive-check (`const x: never = msg`) only works
          // when ServerMessage is a true multi-variant union. Add it once a second
          // message type is added to the union in types/messages.ts.
          break
      }
    }

    // Cleanup: close the socket when the component unmounts or url changes.
    return () => {
      ws.close()
    }
  }, [url])

  return state
}
