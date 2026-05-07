import { useState, useEffect } from 'react'
import { useNavigate } from 'react-router-dom'
import { GenerateKeyModal } from '../components/GenerateKeyModal'
import type { ApiKeyView, ApiKeyCreateResponse } from '../types/messages'
import './ApiKeySettings.css'

function isActive(key: ApiKeyView): boolean {
  if (key.revoked_at) return false
  return new Date(key.expires_at) > new Date()
}

function keyStatus(key: ApiKeyView): 'active' | 'expired' | 'revoked' {
  if (key.revoked_at) return 'revoked'
  if (new Date(key.expires_at) <= new Date()) return 'expired'
  return 'active'
}

export function ApiKeySettings() {
  const navigate = useNavigate()
  const [keys,       setKeys]      = useState<ApiKeyView[]>([])
  const [loading,    setLoading]   = useState(true)
  const [nameInput,  setNameInput] = useState('')
  const [generating, setGenerating] = useState(false)
  const [error,      setError]     = useState<string | null>(null)
  const [modalKey,   setModalKey]  = useState<{ key: string; name: string } | null>(null)

  useEffect(() => {
    fetch('/players/me/api-keys', { credentials: 'include' })
      .then(r => r.json())
      .then((data: ApiKeyView[]) => setKeys(data))
      .catch(() => setError('Failed to load API keys'))
      .finally(() => setLoading(false))
  }, [])

  const hasActiveKey = keys.some(isActive)

  async function handleGenerate(e: React.FormEvent) {
    e.preventDefault()
    if (!nameInput.trim() || hasActiveKey) return
    setGenerating(true)
    setError(null)
    try {
      const res = await fetch('/players/me/api-keys', {
        method:      'POST',
        credentials: 'include',
        headers:     { 'Content-Type': 'application/json' },
        body:        JSON.stringify({ name: nameInput.trim() }),
      })
      if (res.status === 409) {
        setError('You already have an active key. Revoke it first.')
        return
      }
      if (!res.ok) {
        const body = await res.json().catch(() => ({}))
        setError((body as { message?: string }).message ?? 'Failed to generate key')
        return
      }
      const data: ApiKeyCreateResponse = await res.json()
      setModalKey({ key: data.key, name: data.name })
      setKeys(prev => [...prev, {
        id:         data.id,
        name:       data.name,
        created_at: new Date().toISOString(),
        expires_at: data.expires_at,
        revoked_at: null,
      }])
      setNameInput('')
    } catch {
      setError('Network error')
    } finally {
      setGenerating(false)
    }
  }

  async function handleRevoke(keyId: number) {
    try {
      const res = await fetch(`/players/me/api-keys/${keyId}`, {
        method: 'DELETE', credentials: 'include',
      })
      if (res.ok) {
        setKeys(prev => prev.map(k =>
          k.id === keyId ? { ...k, revoked_at: new Date().toISOString() } : k
        ))
      }
    } catch {
      setError('Failed to revoke key')
    }
  }

  return (
    <div className="ak">
      <header className="ak__header">
        <button className="ak__back" onClick={() => navigate('/lobby')}>← Back</button>
        <h1 className="ak__title">API Keys</h1>
      </header>

      <div className="ak__body">

        {/* ── Generate ─────────────────────────────────────────────────── */}
        <div className="ak__section">
          <span className="ak__section-label">New Key</span>
          <form className="ak__generate-row" onSubmit={handleGenerate}>
            <input
              className="ak__name-input"
              type="text"
              placeholder="Key name (e.g. my-bot)"
              value={nameInput}
              onChange={e => setNameInput(e.target.value)}
              maxLength={100}
              disabled={hasActiveKey || generating}
            />
            <button
              className="ak__btn ak__btn--generate"
              type="submit"
              disabled={!nameInput.trim() || hasActiveKey || generating}
            >
              {generating ? 'Generating…' : 'Generate'}
            </button>
          </form>
          {hasActiveKey
            ? <p className="ak__limit-note">Revoke your existing key before generating a new one.</p>
            : <p className="ak__limit-note">One active key per account · 30-day expiry</p>
          }
          {error && <p className="ak__error">{error}</p>}
        </div>

        {/* ── Key list ─────────────────────────────────────────────────── */}
        <div className="ak__list-section">
          <span className="ak__section-label">Your Keys</span>
          {loading ? (
            <p className="ak__empty">Loading…</p>
          ) : keys.length === 0 ? (
            <p className="ak__empty">No keys yet — generate one above to connect scripts.</p>
          ) : (
            <ul className="ak__list">
              {keys.map(k => {
                const status = keyStatus(k)
                return (
                  <li key={k.id} className={`ak__card${status !== 'active' ? ' ak__card--inactive' : ''}`}>
                    <div className="ak__card-left">
                      <span className="ak__card-name">{k.name}</span>
                      <div className="ak__card-meta">
                        <span className={`ak__badge ak__badge--${status}`}>
                          {status}
                        </span>
                        {status === 'active'
                          ? `Expires ${new Date(k.expires_at).toLocaleDateString()}`
                          : status === 'expired'
                            ? `Expired ${new Date(k.expires_at).toLocaleDateString()}`
                            : `Revoked ${new Date(k.revoked_at!).toLocaleDateString()}`
                        }
                      </div>
                    </div>
                    {status === 'active' && (
                      <button
                        className="ak__btn--revoke"
                        onClick={() => handleRevoke(k.id)}
                      >
                        Revoke
                      </button>
                    )}
                  </li>
                )
              })}
            </ul>
          )}
        </div>

      </div>

      {modalKey && (
        <GenerateKeyModal
          apiKey={modalKey.key}
          keyName={modalKey.name}
          onClose={() => setModalKey(null)}
        />
      )}
    </div>
  )
}
