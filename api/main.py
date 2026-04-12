import os
from pathlib import Path
from contextlib import asynccontextmanager
from enum import Enum
from fastapi import FastAPI, Security, HTTPException
from fastapi.responses import JSONResponse, HTMLResponse
from fastapi.security.api_key import APIKeyHeader
from pydantic import BaseModel
from dotenv import load_dotenv
from sqlalchemy.ext.asyncio import (
    create_async_engine,
    async_sessionmaker,
    AsyncSession
)
from sqlalchemy.orm import DeclarativeBase
from sqlalchemy import Column, Float, DateTime, Index, func, select

# load timescaledb/.env
TSDB_DOTENV = Path(__file__).parent.parent / "timescaledb" / ".env"
if not TSDB_DOTENV.exists():
    raise FileNotFoundError(f"Missing .env file at {TSDB_DOTENV}")
load_dotenv(TSDB_DOTENV)

# load ./.env
DOTENV = Path(__file__).parent / ".env"
if not DOTENV.exists():
    raise FileNotFoundError(f"Missing .env file at {DOTENV}")
load_dotenv(DOTENV)

PAGES = Path(__file__).parent / "pages"

PG_USER = os.getenv("POSTGRES_USER", None)
PG_PASSWORD = os.getenv("POSTGRES_PASSWORD", None)
PG_HOST = os.getenv("POSTGRES_HOST", None)
PG_PORT = os.getenv("POSTGRES_PORT", None)
PG_DB = os.getenv("POSTGRES_DB", None)

API_KEY = os.getenv("API_KEY", None)

if not all([PG_USER, PG_PASSWORD, PG_HOST, PG_PORT, PG_DB]):
    raise ValueError(
        "Missing one or more required environment variables: "
        "POSTGRES_USER, POSTGRES_PASSWORD, POSTGRES_HOST, "
        "POSTGRES_PORT, POSTGRES_DB. These should be defined in "
        "`SensorProject/timescaledb/.env`."
    )

if not API_KEY:
    raise ValueError("Missing API_KEY in SensorProject/api/.env")

DATABASE_URL = "postgresql+asyncpg://{USER}:{PASSWORD}@{HOST}:{PORT}/{DB}".format(  # noqa: E501
    USER=PG_USER,
    PASSWORD=PG_PASSWORD,
    HOST=PG_HOST,
    PORT=PG_PORT,
    DB=PG_DB
)

engine = create_async_engine(DATABASE_URL, echo=False)
AsyncSessionLocal = async_sessionmaker(
    engine,
    expire_on_commit=False,
    class_=AsyncSession
)


class Base(DeclarativeBase):
    pass


class OutdoorMeasurement(Base):
    __tablename__ = "outdoor"
    __table_args__ = (
        Index("idx_outdoor_time", "time", postgresql_using="brin"),
    )
    time = Column(
        DateTime(timezone=True),
        primary_key=True,
        server_default=func.now()
    )
    temperature = Column(Float)
    humidity = Column(Float)
    pressure = Column(Float)
    light_level = Column(Float)
    battery_voltage = Column(Float)
    espnow_strength_dbm = Column(Float)


class IndoorMeasurement(Base):
    __tablename__ = "indoor"
    __table_args__ = (
        Index("idx_indoor_time", "time", postgresql_using="brin"),
    )
    time = Column(
        DateTime(timezone=True),
        primary_key=True,
        server_default=func.now()
    )
    temperature = Column(Float)
    humidity = Column(Float)
    pressure = Column(Float)
    gas = Column(Float)
    pm1_0 = Column(Float)
    pm2_5 = Column(Float)
    pm10_0 = Column(Float)
    pm_bin_03_05 = Column(Float)
    pm_bin_05_10 = Column(Float)
    pm_bin_10_25 = Column(Float)
    pm_bin_25_50 = Column(Float)
    pm_bin_50_100 = Column(Float)
    pm_bin_100_plus = Column(Float)
    vocs = Column(Float)
    carbon_monoxide = Column(Float)
    carbon_dioxide = Column(Float)
    wifi_strength = Column(Float)


class OutdoorData(BaseModel):
    temperature: float | None = None
    humidity: float | None = None
    pressure: float | None = None
    light_level: float | None = None
    battery_voltage: float | None = None
    espnow_strength_dbm: float | None = None


class PMBins(BaseModel):
    bin_03_05: float | None = None
    bin_05_10: float | None = None
    bin_10_25: float | None = None
    bin_25_50: float | None = None
    bin_50_100: float | None = None
    bin_100_plus: float | None = None


class IndoorData(BaseModel):
    temperature: float | None = None
    humidity: float | None = None
    pressure: float | None = None
    gas: float | None = None
    pm1_0: float | None = None
    pm2_5: float | None = None
    pm10_0: float | None = None
    pm_bins: PMBins | None = None
    vocs: float | None = None
    carbon_monoxide: float | None = None
    carbon_dioxide: float | None = None
    wifi_strength: float | None = None


@asynccontextmanager
async def lifespan(app: FastAPI):
    yield
    await engine.dispose()


app = FastAPI(lifespan=lifespan)

api_key_header = APIKeyHeader(name="X-API-Key")


async def verify_api_key(api_key: str = Security(api_key_header)):
    if api_key != API_KEY:
        raise HTTPException(status_code=403, detail="Invalid API Key")


class Table(Enum):
    INDOOR = "indoor"
    OUTDOOR = "outdoor"

    @classmethod
    def from_str(cls, val: str) -> "Table | None":
        val = val.lower()

        for member in cls:
            if member.value == val:
                return member

        return None


async def insert_data(table: Table, data: dict) -> None:
    match table:
        case Table.OUTDOOR:
            model = OutdoorMeasurement
        case Table.INDOOR:
            model = IndoorMeasurement
        case _:
            raise ValueError("Invalid table")  # how tf did that happen :sob:

    async with AsyncSessionLocal() as session:
        async with session.begin():
            obj = model(**data)
            session.add(obj)


@app.post("/update/outdoor", dependencies=[Security(verify_api_key)])
async def update_outdoor(
    data: OutdoorData
) -> JSONResponse:
    # -------------------------------------------------
    #  Outdoor ESP32
    # -------------------------------------------------
    #  Metrics from outdoor ESP32:
    #  Temp/Humidity/Pressure Sensor (BME280) Readouts
    #  Light Level Sensor (BH1750) Readouts
    #  ESP32 Health Metrics
    # -------------------------------------------------

    table = Table.OUTDOOR
    valid_data = {k: v for k, v in data.model_dump().items() if v is not None}

    if valid_data:
        try:
            await insert_data(table, valid_data)
        except Exception as e:
            print(f"Error updating {table.value} data: {e}")
            return JSONResponse(status_code=500, content={
                "status": "error",
                "message": str(e)
            })

    return JSONResponse(status_code=200, content={
        "status": "pushed",
        "table": table.value,
    })


@app.post("/update/indoor", dependencies=[Security(verify_api_key)])
async def update_indoor(
    data: IndoorData
) -> JSONResponse:
    # -------------------------------------------------
    #  Indoor ESP32
    # -------------------------------------------------
    #  Metrics from indoor ESP32:
    #  Temperature/Humidity/Pressure/Gas (BME680) Readouts
    #  Particulate matter (PMS5003) Readouts
    #  VOCs (SGP40) Readouts
    #  Carbon Monoxide (MQ-7) Readouts
    #  Carbon Dioxide (SCD41) Readouts
    #  ESP32 Health Metrics
    # -------------------------------------------------

    table = Table.INDOOR
    valid_data: dict[str, float] = {}

    for k, v in data.model_dump().items():
        if k == "pm_bins" and v is not None:
            v: dict[str, float]
            for b in [
                "bin_03_05", "bin_05_10", "bin_10_25",
                "bin_25_50", "bin_50_100", "bin_100_plus"
            ]:
                if (val := v.get(b)) is not None:
                    valid_data[f"pm_{b}"] = val
        elif v is not None:
            valid_data[k] = v  # type: ignore

    if valid_data:
        try:
            await insert_data(table, valid_data)
        except Exception as e:
            print(f"Error updating {table.value} data: {e}")
            return JSONResponse(status_code=500, content={
                "status": "error",
                "message": str(e)
            })

    return JSONResponse(status_code=200, content={
        "status": "pushed",
        "table": table.value,
    })


@app.get("/")
async def root():
    with open(PAGES / "root.html", "r") as f:
        html = f.read()

    return HTMLResponse(
        status_code=200,
        content=html
    )


@app.get("/health")
async def health() -> JSONResponse:
    return JSONResponse(status_code=200, content={
        "status": "ok"
    })


@app.get("/latest/indoor")
async def get_latest_indoor():
    async with AsyncSessionLocal() as session:
        stmt = (
            select(IndoorMeasurement)
            .order_by(IndoorMeasurement.time.desc())
            .limit(1)
        )
        result = await session.execute(stmt)
        obj = result.scalar_one_or_none()

        if not obj:
            return JSONResponse(
                status_code=404,
                content={"status": "error", "message": "No data found"}
            )

        data = {c.name: getattr(obj, c.name) for c in obj.__table__.columns}
        if data.get("time"):
            data["time"] = data["time"].isoformat()

        return JSONResponse(status_code=200, content=data)


@app.get("/latest/indoor/html")
async def get_latest_indoor_html():
    async with AsyncSessionLocal() as session:
        stmt = (
            select(IndoorMeasurement)
            .order_by(IndoorMeasurement.time.desc())
            .limit(1)
        )
        result = await session.execute(stmt)
        obj = result.scalar_one_or_none()

        if not obj:
            return HTMLResponse(
                status_code=404,
                content="<h1>No data found</h1>"
            )

        html_content = "<h1>Latest Indoor Metrics</h1><ul>"
        for c in obj.__table__.columns:
            val = getattr(obj, c.name)
            html_content += f"<li><b>{c.name}:</b> {val}</li>"
        html_content += "</ul>"

        return HTMLResponse(status_code=200, content=html_content)


@app.get("/latest/outdoor")
async def get_latest_outdoor():
    async with AsyncSessionLocal() as session:
        stmt = (
            select(OutdoorMeasurement)
            .order_by(OutdoorMeasurement.time.desc())
            .limit(1)
        )
        result = await session.execute(stmt)
        obj = result.scalar_one_or_none()

        if not obj:
            return JSONResponse(
                status_code=404,
                content={"status": "error", "message": "No data found"}
            )

        data = {c.name: getattr(obj, c.name) for c in obj.__table__.columns}
        if data.get("time"):
            data["time"] = data["time"].isoformat()

        return JSONResponse(status_code=200, content=data)


@app.get("/latest/outdoor/html")
async def get_latest_outdoor_html():
    async with AsyncSessionLocal() as session:
        stmt = (
            select(OutdoorMeasurement)
            .order_by(OutdoorMeasurement.time.desc())
            .limit(1)
        )
        result = await session.execute(stmt)
        obj = result.scalar_one_or_none()

        if not obj:
            return HTMLResponse(
                status_code=404,
                content="<h1>No data found</h1>"
            )

        html_content = "<h1>Latest Outdoor Metrics</h1><ul>"
        for c in obj.__table__.columns:
            val = getattr(obj, c.name)
            html_content += f"<li><b>{c.name}:</b> {val}</li>"
        html_content += "</ul>"

        return HTMLResponse(status_code=200, content=html_content)


if __name__ == "__main__":
    import uvicorn
    uvicorn.run(app, host=str(os.getenv("BIND_TO", "0.0.0.0")), port=8080)
