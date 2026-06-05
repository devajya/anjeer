import { KeybindSettings } from './KeybindSettings'
import './SettingsPage.css'

export function SettingsPage() {
  return (
    <div className="settings-page">
      <div className="settings-page__content">
        <KeybindSettings />
      </div>
    </div>
  )
}
