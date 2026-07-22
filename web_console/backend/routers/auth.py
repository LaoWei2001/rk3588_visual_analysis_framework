from fastapi import APIRouter, HTTPException, Security
from fastapi.security import HTTPBearer, HTTPAuthorizationCredentials
from pydantic import BaseModel
from typing import Optional

from services.auth_service import verify_credentials, create_session, revoke_session, get_session

router = APIRouter(tags=["auth"])
_bearer = HTTPBearer(auto_error=False)


class LoginRequest(BaseModel):
    username: str
    password: str


class LoginResponse(BaseModel):
    token: str
    username: str


@router.post("/auth/login", response_model=LoginResponse)
async def login(req: LoginRequest):
    if not req.username or not req.password:
        raise HTTPException(status_code=400, detail="用户名和密码不能为空")
    if not verify_credentials(req.username, req.password):
        raise HTTPException(status_code=401, detail="用户名或密码错误")
    token = create_session(req.username)
    return LoginResponse(token=token, username=req.username)


@router.post("/auth/logout")
async def logout(creds: Optional[HTTPAuthorizationCredentials] = Security(_bearer)):
    if creds:
        revoke_session(creds.credentials)
    return {"ok": True}


@router.get("/auth/me")
async def me(creds: Optional[HTTPAuthorizationCredentials] = Security(_bearer)):
    if not creds:
        raise HTTPException(status_code=401, detail="Not authenticated")
    session = get_session(creds.credentials)
    if not session:
        raise HTTPException(status_code=401, detail="Token 已失效，请重新登录")
    return {"username": session["username"]}
