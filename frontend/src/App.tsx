import { useWebSocket } from './hooks/useWebSocket'
import { ConnectionBanner } from './components/ConnectionBanner'
import './App.css'

function App() {
  // AGENT-CTX: '/ws' is a relative path proxied by Vite to ws://localhost:9001/ws in dev.
  // For production, nginx proxies /ws to the backend WebSocket server.
  // See vite.config.ts and contexts/machine-setup.md AGENT_CTX note.
  const { connected, lastServerTs } = useWebSocket('/ws')

  return (
    <main className="app">
      <h1 className="app__title">Anjeer</h1>
      <ConnectionBanner connected={connected} lastServerTs={lastServerTs} />
    </main>
  )
}

export default App
