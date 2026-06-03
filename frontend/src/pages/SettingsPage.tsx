import { useLocation } from 'react-router-dom'
import { NavLink } from 'react-router-dom'
import { KeybindSettings } from './KeybindSettings'
import { FeedPreferenceSection } from '../components/FeedPreferenceSection'
import './SettingsPage.css'

export function SettingsPage() {
  const location = useLocation()
  const isPrefs = location.pathname.startsWith('/settings/preferences')

  return (
    <div className="settings-page">
      <nav className="settings-page__nav" aria-label="Settings subpages">
        <NavLink
          to="/settings/keybinds"
          className={({ isActive }) =>
            ['settings-page__tab', isActive ? 'settings-page__tab--active' : ''].join(' ').trim()
          }
        >
          Keybinds
        </NavLink>
        <NavLink
          to="/settings/preferences"
          className={({ isActive }) =>
            ['settings-page__tab', isActive ? 'settings-page__tab--active' : ''].join(' ').trim()
          }
        >
          Preferences
        </NavLink>
      </nav>

      <div className="settings-page__content">
        {isPrefs ? <FeedPreferenceSection /> : <KeybindSettings />}
      </div>
    </div>
  )
}
