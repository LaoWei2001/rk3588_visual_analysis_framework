"""
Session-based authentication backed by Linux system credentials.

Verification order:
  1. python-pam  (preferred – uses system PAM stack)
  2. spwd + crypt (fallback for Python ≤ 3.12, requires root)
Authentication fails closed if neither verifier is available. Running su as
root does not verify the supplied password and must never be used for login.
"""
import logging
import secrets
import time
from typing import Optional

logger = logging.getLogger(__name__)

# ── In-memory session store ────────────────────────────────────────────────
#   token (hex-64) → { username, expires_at }
_sessions: dict[str, dict] = {}
SESSION_TTL = 8 * 3600  # 8 hours


def verify_credentials(username: str, password: str) -> bool:
    """Return True if the Linux system accepts this username/password."""

    # ── Method 1: python-pam ──────────────────────────────────────────────
    try:
        import pam  # type: ignore
    except ImportError:
        logger.debug("python-pam not installed, trying spwd fallback")
    else:
        try:
            p = pam.pam()
            return bool(p.authenticate(username, password, service="login"))
        except Exception as exc:
            logger.warning("PAM auth error: %s", exc)
            return False  # Do not bypass a failing PAM policy with another verifier.

    # ── Method 2: spwd + crypt (Python ≤ 3.12, service must run as root) ──
    try:
        import spwd   # type: ignore  # removed in Python 3.13
        import crypt  # type: ignore  # removed in Python 3.13
        try:
            sp = spwd.getspnam(username)
        except (KeyError, PermissionError):
            return False
        hashed = sp.sp_pwdp
        if not hashed or hashed.startswith(('!', '*')):
            return False
        today = int(time.time() // 86400)
        if sp.sp_expire >= 0 and today >= sp.sp_expire:
            return False
        if sp.sp_lstchg == 0 or (sp.sp_max >= 0 and today > sp.sp_lstchg + sp.sp_max):
            return False
        candidate = crypt.crypt(password, hashed)
        return bool(candidate) and secrets.compare_digest(candidate, hashed)
    except Exception as exc:
        logger.error("No working credential verifier; login denied: %s", exc)

    return False


# ── Session management ─────────────────────────────────────────────────────

def create_session(username: str) -> str:
    token = secrets.token_hex(32)
    _sessions[token] = {
        "username": username,
        "expires_at": time.monotonic() + SESSION_TTL,
    }
    logger.info("Session created for '%s'", username)
    return token


def get_session(token: str) -> Optional[dict]:
    session = _sessions.get(token)
    if not session:
        return None
    # Manual clock changes must not expire sessions or extend their lifetime.
    if time.monotonic() > session["expires_at"]:
        del _sessions[token]
        return None
    return session


def revoke_session(token: str) -> None:
    if token in _sessions:
        username = _sessions[token].get("username", "?")
        del _sessions[token]
        logger.info("Session revoked for '%s'", username)
