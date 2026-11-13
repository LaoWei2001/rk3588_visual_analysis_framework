"""Shared HTTP authorization dependencies, independent of individual routers."""

import grp
import os
import pwd

from fastapi import HTTPException, Request


def require_system_admin(request: Request) -> None:
    """Restrict system administration to root, sudo or wheel accounts."""
    session = getattr(request.state, "session", {})
    username = str(session.get("username", ""))
    try:
        account = pwd.getpwnam(username)
        group_ids = os.getgrouplist(username, account.pw_gid)
        group_names = {grp.getgrgid(group_id).gr_name for group_id in group_ids}
    except (KeyError, OSError):
        raise HTTPException(403, "无法确认当前账号的系统管理权限")
    if account.pw_uid != 0 and not group_names.intersection({"sudo", "wheel"}):
        raise HTTPException(403, "此操作仅允许 root、sudo 或 wheel 管理员账号执行")
