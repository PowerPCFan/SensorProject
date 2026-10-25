import asyncio
from collections.abc import AsyncGenerator
from contextlib import asynccontextmanager
from re import I
from typing import cast

import httpx
from fastapi import FastAPI, HTTPException, Request
from fastapi.middleware.cors import CORSMiddleware
from fastapi.responses import JSONResponse
from slowapi import Limiter, _rate_limit_exceeded_handler
from slowapi.errors import RateLimitExceeded


@asynccontextmanager
async def lifespan(app: FastAPI) -> AsyncGenerator[None]:
    app.state.client = httpx.AsyncClient(timeout=5.0)
    yield
    await app.state.client.aclose()


def get_remote_address(request: Request) -> str:
    if head := request.headers.get(
        "cf-connecting-ip",
        request.headers.get(
            "x-forwarded-for",
            request.headers.get(
                "x-real-ip",
                None,
            ),
        ),
    ):
        if "," in head:
            # parse weird ip lists that are sometimes sent in headers
            return head.split(",")[0].strip()

        return head

    if not request.client or not request.client.host:
        return "127.0.0.1"

    return request.client.host


limiter = Limiter(key_func=get_remote_address)
app = FastAPI(
    lifespan=lifespan,
    docs_url=None,
    redoc_url=None,
    openapi_url=None,
)
app.state.limiter = limiter
app.add_exception_handler(RateLimitExceeded, _rate_limit_exceeded_handler)  # pyright: ignore[reportArgumentType]

app.add_middleware(
    CORSMiddleware,
    allow_origins=[
        "https://www.powerpcfan.xyz",
        "http://localhost:5173",
    ],
    allow_credentials=True,
    allow_methods=["GET"],
    allow_headers=["*"],
)


@app.get("/")
@limiter.limit("10/minute")
async def get_latest(request: Request) -> JSONResponse:
    try:
        client = cast("httpx.AsyncClient", request.app.state.client)
        in_res, out_res = await asyncio.gather(
            client.get("http://127.0.0.1:8080/latest/indoor"),
            client.get("http://127.0.0.1:8080/latest/outdoor"),
        )

        if in_res.status_code != 200 or out_res.status_code != 200:  # noqa: PLR2004
            raise HTTPException(status_code=502, detail="Error retrieving sensor data.")

        indoor: dict[str, str | int | float | None] = in_res.json()
        outdoor: dict[str, str | int | float | None] = out_res.json()

        return JSONResponse(
            content={
                "indoor": {
                    "time": indoor.get("time"),
                    "temperature": indoor.get("temperature"),
                    "humidity": indoor.get("humidity"),
                    "pressure": indoor.get("pressure"),
                    "gas": indoor.get("gas"),
                    "vocs": indoor.get("vocs"),
                    "carbon_dioxide": indoor.get("carbon_dioxide"),
                    "wifi_strength": indoor.get("wifi_strength"),
                },
                "outdoor": {
                    "time": outdoor.get("time"),
                    "temp": outdoor.get("temperature"),
                    "hum": outdoor.get("humidity"),
                    "pressure": outdoor.get("pressure"),
                    "dew_point": outdoor.get("dew_point"),
                    "internal_temp": outdoor.get("internal_temperature"),
                    "internal_hum": outdoor.get("internal_humidity"),
                    "esp32_temp": outdoor.get("esp32_temperature"),
                    "esp32_voltage": outdoor.get("esp32_voltage"),
                    "signal_strength": outdoor.get("wifi_strength"),
                },
            },
            status_code=200,
        )

    except httpx.RequestError:
        raise HTTPException(status_code=504, detail="Error retrieving sensor data.")


@app.get("/health")
async def health_check() -> JSONResponse:
    return JSONResponse(content={"status": "ok", "sog": True}, status_code=200)


if __name__ == "__main__":
    import uvicorn

    uvicorn.run(app, host="0.0.0.0", port=9191)  # ruff: ignore[S104]
