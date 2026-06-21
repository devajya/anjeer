import { NavLink, useNavigate } from 'react-router-dom'
import { useAuth } from '../hooks/useAuth'
import './AppNav.css'

export function AppNav() {
  const { user, logout } = useAuth()
  const navigate = useNavigate()

  function handleLogout() {
    logout()
    navigate('/auth')
  }

  return (
    <nav className="app-nav" aria-label="Main navigation">
      <NavLink to="/" className="app-nav__wordmark" aria-label="Anjeer home">
        Anjeer
      </NavLink>

      <div className="app-nav__links">
        <NavLink
          to="/lobby"
          end={false}
          className={({ isActive }) =>
            `app-nav__link${isActive ? ' app-nav__link--active' : ''}`
          }
        >
          Lobby
        </NavLink>
        <NavLink
          to="/rules"
          className={({ isActive }) =>
            `app-nav__link${isActive ? ' app-nav__link--active' : ''}`
          }
        >
          Rules
        </NavLink>
        <NavLink
          to="/docs"
          className={({ isActive }) =>
            `app-nav__link${isActive ? ' app-nav__link--active' : ''}`
          }
        >
          Docs
        </NavLink>
        <NavLink
          to="/api-keys"
          className={({ isActive }) =>
            `app-nav__link${isActive ? ' app-nav__link--active' : ''}`
          }
        >
          API Keys
        </NavLink>
        <NavLink
          to="/settings"
          className={({ isActive }) =>
            `app-nav__link${isActive ? ' app-nav__link--active' : ''}`
          }
        >
          Settings
        </NavLink>
        <NavLink
          to="/learn"
          className={({ isActive }) =>
            `app-nav__link${isActive ? ' app-nav__link--active' : ''}`
          }
        >
          Math
        </NavLink>
      </div>

      <div className="app-nav__user">
        {user && <span className="app-nav__username">{user.username}</span>}
        <button className="app-nav__logout" onClick={handleLogout} aria-label="Sign out">
          Sign out
        </button>
      </div>
    </nav>
  )
}
