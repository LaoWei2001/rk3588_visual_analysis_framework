from services import auth_service as auth


def test_clock_changes_do_not_expire_sessions_or_extend_ttl(monkeypatch):
    clock = {'monotonic': 100., 'wall': 1700000000.}
    monkeypatch.setattr(auth.time, 'monotonic', lambda: clock['monotonic'])
    monkeypatch.setattr(auth.time, 'time', lambda: clock['wall'])
    token = auth.create_session('clock-test')
    try:
        clock['wall'] += 365 * 86400
        assert auth.get_session(token)['username'] == 'clock-test'
        clock['wall'] -= 2 * 365 * 86400
        clock['monotonic'] += auth.SESSION_TTL + 1
        assert auth.get_session(token) is None
    finally:
        auth.revoke_session(token)
