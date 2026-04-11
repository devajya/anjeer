import { defineConfig } from 'vite'
import react from '@vitejs/plugin-react'

// AGENT-CTX: The WS proxy maps /ws → ws://localhost:9001/ws so the frontend
// uses a relative URL and the Vite dev server handles the tunnel.
// For production, replace this with nginx `proxy_pass` on /ws.
// The port (9001) must match server.port in config/default.json.
// If the port changes, update both this file and config/default.json.
// See contexts/machine-setup.md for the nginx production note.
export default defineConfig({
  plugins: [react()],
  server: {
    proxy: {
      '/ws': {
        target: 'ws://localhost:9001',
        ws: true,
        // AGENT-CTX: changeOrigin=false — uWS in Slice 1 does not check the
        // Host header. Set to true if CORS rejections appear in later slices.
        changeOrigin: false,
      },
    },
  },
  test: {
    // AGENT-CTX: jsdom simulates a browser DOM so React components and
    // window.location-based URL construction in useWebSocket work without a real browser.
    globals: true,
    environment: 'jsdom',
    setupFiles: ['./src/test-setup.ts'],
  },
})
