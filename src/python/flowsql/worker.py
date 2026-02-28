"""FlowSQL Python Worker — FastAPI 应用"""

import asyncio
import os

from fastapi import FastAPI, Request, Response, HTTPException
import uvicorn

from .arrow_codec import decode_arrow_ipc, encode_arrow_ipc
from .config import WorkerConfig
from .control_client import ControlClient
from .operator_registry import OperatorRegistry

app = FastAPI(title="FlowSQL Python Worker")
registry = OperatorRegistry()
control_client: ControlClient = None


def _do_reload():
    """重载算子的共享逻辑（问题 7）"""
    config = WorkerConfig.from_args()
    old_keys = set(f"{op['catelog']}.{op['name']}" for op in registry.list_operators())

    registry.reload(config.operators_dir)

    new_keys = set(f"{op['catelog']}.{op['name']}" for op in registry.list_operators())

    added = new_keys - old_keys
    removed = old_keys - new_keys

    print(f"Worker _do_reload: added={len(added)}, removed={len(removed)}")

    if control_client:
        for key in added:
            op = registry.get(*key.split(".", 1))
            if op:
                attr = op.attribute()
                ok = control_client.send_operator_added(attr.catelog, attr.name, attr.description, attr.position)
                if ok:
                    print(f"Worker: sent operator_added: {key}")
                else:
                    print(f"Worker: FAILED to send operator_added: {key}")
        for key in removed:
            parts = key.split(".", 1)
            ok = control_client.send_operator_removed(parts[0], parts[1])
            if ok:
                print(f"Worker: sent operator_removed: {key}")
            else:
                print(f"Worker: FAILED to send operator_removed: {key}")
    else:
        print("Worker: control_client not connected, skipping notifications")

    return added, removed


@app.get("/health")
async def health():
    return {"status": "ok"}


@app.get("/operators")
async def list_operators():
    return registry.list_operators()


@app.post("/reload")
async def reload_operators():
    """重新扫描算子目录，发现新增/移除的算子并通知 C++ 端"""
    loop = asyncio.get_event_loop()
    added, removed = await loop.run_in_executor(None, _do_reload)
    return {"added": list(added), "removed": list(removed)}


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
    global control_client

    config = WorkerConfig.from_args()

    # 1. 连接到 C++ 控制服务器
    control_socket = os.environ.get('FLOWSQL_CONTROL_SOCKET')
    if not control_socket:
        print("ERROR: FLOWSQL_CONTROL_SOCKET environment variable not set")
        return

    control_client = ControlClient(control_socket)
    if not control_client.connect(timeout=5):
        print("ERROR: Failed to connect to control server")
        control_client = None
        return

    # 2. 注册命令处理器
    def handle_reload_operators(payload):
        """收到 C++ 端的重载命令，重新扫描算子目录"""
        print("Worker: received reload_operators command")
        _do_reload()

    def handle_shutdown(payload):
        print("Worker: received shutdown command")
        # TODO: 优雅关闭

    control_client.register_handler("reload_operators", handle_reload_operators)
    control_client.register_handler("shutdown", handle_shutdown)

    # 3. 发现并注册算子
    registry.discover(config.operators_dir)

    # 4. 发送就绪通知
    operators = registry.list_operators()
    if not control_client.send_worker_ready(operators):
        print("ERROR: Failed to send worker_ready message")
        control_client.disconnect()
        return

    print(f"FlowSQL Worker: sent ready notification with {len(operators)} operators")

    # 5. 启动消息接收循环（异步）
    control_client.start_message_loop()

    # 6. 启动 HTTP 服务
    print(f"FlowSQL Worker starting on {config.host}:{config.port}")
    print(f"Operators directory: {config.operators_dir}")
    print(f"Registered operators: {len(operators)}")

    try:
        uvicorn.run(app, host=config.host, port=config.port, log_level="warning")
    finally:
        # 清理
        if control_client:
            control_client.disconnect()


if __name__ == "__main__":
    main()
