"""FlowSQL Python Worker — FastAPI 应用"""

import os
import sys

from fastapi import FastAPI, Request, Response, HTTPException
import uvicorn

from .arrow_codec import decode_arrow_ipc, encode_arrow_ipc
from .config import WorkerConfig
from .operator_registry import OperatorRegistry

app = FastAPI(title="FlowSQL Python Worker")
registry = OperatorRegistry()


@app.get("/health")
async def health():
    return {"status": "ok"}


@app.get("/operators")
async def list_operators():
    return registry.list_operators()


@app.post("/work/{catelog}/{name}")
async def work(catelog: str, name: str, request: Request):
    op = registry.get(catelog, name)
    if not op:
        raise HTTPException(status_code=404, detail=f"Operator {catelog}.{name} not found")

    body = await request.body()
    if not body:
        raise HTTPException(status_code=400, detail="Empty request body")

    try:
        df_in = decode_arrow_ipc(body)
    except Exception as e:
        raise HTTPException(status_code=400, detail=f"Failed to decode Arrow IPC: {e}")

    try:
        df_out = op.work(df_in)
    except Exception as e:
        raise HTTPException(status_code=500, detail=f"Operator error: {e}")

    try:
        result = encode_arrow_ipc(df_out)
    except Exception as e:
        raise HTTPException(status_code=500, detail=f"Failed to encode Arrow IPC: {e}")

    return Response(content=result, media_type="application/vnd.apache.arrow.stream")


@app.post("/configure/{catelog}/{name}")
async def configure(catelog: str, name: str, request: Request):
    body = await request.json()
    key = body.get("key", "")
    value = body.get("value", "")

    if not registry.configure(catelog, name, key, value):
        raise HTTPException(status_code=404, detail=f"Operator {catelog}.{name} not found")

    return {"status": "ok"}


def main():
    config = WorkerConfig.from_args()

    # 发现并注册算子
    registry.discover(config.operators_dir)

    print(f"FlowSQL Worker starting on {config.host}:{config.port}")
    print(f"Operators directory: {config.operators_dir}")
    print(f"Registered operators: {len(registry.list_operators())}")

    uvicorn.run(app, host=config.host, port=config.port, log_level="warning")


if __name__ == "__main__":
    main()
