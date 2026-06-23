"""
Session-based authentication backed by Linux system credentials.

Verification order:
  1. python-pam  (preferred – uses system PAM stack)
  2. spwd + crypt (fallback for Python ≤ 3.12, requires root)
  3. subprocess su (last-resort fallback)
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
        p = pam.pam()
        return bool(p.authenticate(username, password, service="login"))
    except ImportError:
        logger.debug("python-pam not installed, trying spwd fallback")
    except Exception as exc:
        logger.warning("PAM auth error: %s", exc)

    # ── Method 2: spwd + crypt (Python ≤ 3.12, service must run as root) ──
    try:
        import spwd   # type: ignore  # removed in Python 3.13
        import crypt  # type: ignore  # removed in Python 3.13
        try:
            sp = spwd.getspnam(username)
        except (KeyError, PermissionError):
            return False
        hashed = sp.sp_pwdp
        return crypt.crypt(password, hashed) == hashed
    except (ImportError, AttributeError):
        logger.debug("spwd/crypt not available, trying su fallback")

    # ── Method 3: subprocess su ──────────────────────────────────────────
    try:
        import subprocess
        proc = subprocess.Popen(
            ["su", "-s", "/bin/sh", "-c", "exit 0", "--", username],
            stdin=subprocess.PIPE,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        try:
            proc.communicate(input=(password + "\n").encode(), timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
            return False
        return proc.returncode == 0
    except Exception as exc:
        logger.error("All credential verification methods failed: %s", exc)

    return False


# ── Session management ─────────────────────────────────────────────────────

def create_session(username: str) -> str:
    token = secrets.token_hex(32)
    _sessions[token] = {
        "username": username,
        "expires_at": time.time() + SESSION_TTL,
    }
    logger.info("Session created for '%s'", username)
    return token


def get_session(token: str) -> Optional[dict]:
    session = _sessions.get(token)
    if not session:
        return None
    if time.time() > session["expires_at"]:
        del _sessions[token]
        return None
    return session


def revoke_session(token: str) -> None:
    if token in _sessions:
        username = _sessions[token].get("username", "?")
        del _sessions[token]
        logger.info("Session revoked for '%s'", username)
