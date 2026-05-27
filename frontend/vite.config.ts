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
  build: {
    rollupOptions: {
      output: {
        // Split heavy 3D/animation vendors so the game UI bundle stays lean.
        // three + R3F are only needed on the landing page Section B.
        manualChunks: {
          'vendor-motion': ['framer-motion'],
          'vendor-gsap':   ['gsap'],
          'vendor-react':  ['react', 'react-dom', 'react-router-dom'],
        },
      },
    },
  },
  server: {
    proxy: {
      '/ws': {
        target: 'ws://localhost:9001',
        ws: true,
        // AGENT-CTX: changeOrigin=false — uWS in Slice 1 does not check the
        // Host header. Set to true if CORS rejections appear in later slices.
        changeOrigin: false,
      },
      // AGENT-CTX: /api/log forwards frontend log batches to the C++ server so
      // they land in logs/frontend_logs.txt alongside server and engine logs.
      // Dev-only endpoint — not exposed in production nginx config.
      '/api': {
        target: 'http://localhost:9001',
        changeOrigin: false,
      },
      // AGENT-CTX: /auth and /players proxy to the Crow HTTP server (:8080), not
      // the uWS game server (:9001). Keep these separate — /api/log is a special
      // endpoint on the WS server and must continue to route to :9001.
      // In production, nginx handles this split at the reverse-proxy level.
      // AGENT-CTX: Regex key ^/auth/ (not bare /auth) so that the React route /auth
      // is served by Vite while OAuth subpaths /auth/github, /auth/google, /auth/*/callback
      // are still forwarded to the HTTP server. Bare /auth without trailing slash
      // would intercept the React route and serve a 404 from the backend.
      '^/auth/': {
        target: 'http://localhost:10000',
        changeOrigin: false,
      },
      '/players': {
        target: 'http://localhost:10000',
        changeOrigin: false,
      },
      '/lobbies': {
        target: 'http://localhost:10000',
        changeOrigin: false,
      },
      '/examples': {
        target: 'http://localhost:10000',
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
    // Needed for ?raw CSS imports (regression guard tests that read raw CSS content).
    css: true,
  },
})
