import { Link } from 'react-router-dom'
import './Footer.css'

export function Footer() {
  return (
    <footer className="site-footer">
      <div className="site-footer__links">
        <Link to="/rules" className="site-footer__link">How to Play</Link>
        <a
          href="https://github.com/devajya"
          target="_blank"
          rel="noopener noreferrer"
          className="site-footer__link"
        >
          GitHub
        </a>
        <a
          href="https://linkedin.com/in/devajya"
          target="_blank"
          rel="noopener noreferrer"
          className="site-footer__link"
        >
          LinkedIn
        </a>
      </div>
      <span className="site-footer__credit">made by devajya</span>
    </footer>
  )
}
