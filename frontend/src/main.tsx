import React from 'react'
import ReactDOM from 'react-dom/client'
import { BrowserRouter } from 'react-router-dom'
import App from './App'
import './index.css'

// AGENT-CTX: BrowserRouter is at the root so the full component tree including
// AuthProvider and all pages can use useNavigate / <Navigate>. Placed here
// (not in App.tsx) so App.tsx remains testable without a real router context.
ReactDOM.createRoot(document.getElementById('root')!).render(
  <React.StrictMode>
    <BrowserRouter>
      <App />
    </BrowserRouter>
  </React.StrictMode>,
)
