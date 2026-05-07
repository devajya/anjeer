import { useState } from 'react'
import './GenerateKeyModal.css'

interface Props {
  apiKey:   string
  keyName:  string
  onClose:  () => void
}

export function GenerateKeyModal({ apiKey, keyName, onClose }: Props) {
  const [copied, setCopied] = useState(false)

  function handleCopy() {
    navigator.clipboard.writeText(apiKey).then(() => {
      setCopied(true)
      setTimeout(() => setCopied(false), 2000)
    })
  }

  return (
    <div className="modal-backdrop" onClick={onClose}>
      <div className="modal-box" onClick={e => e.stopPropagation()}>
        <h2 className="modal-title">API Key Generated</h2>
        <p className="modal-subtitle">
          <strong>{keyName}</strong> — copy this key now. It will not be shown again.
        </p>
        <div className="modal-key-box">
          <code className="modal-key-text">{apiKey}</code>
          <button className="modal-copy-btn" onClick={handleCopy}>
            {copied ? 'Copied!' : 'Copy'}
          </button>
        </div>
        <p className="modal-warning">
          Store this key securely. Once you close this dialog it cannot be retrieved.
        </p>
        <button className="modal-close-btn" onClick={onClose}>Done</button>
      </div>
    </div>
  )
}
