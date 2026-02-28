"""Arrow IPC 编解码"""

import pyarrow as pa
import pandas as pd


def decode_arrow_ipc(data: bytes) -> pd.DataFrame:
    """将 Arrow IPC stream 字节解码为 pandas DataFrame"""
    reader = pa.ipc.open_stream(data)
    try:
        table = reader.read_all()
        return table.to_pandas()
    finally:
        reader.close()


def encode_arrow_ipc(df: pd.DataFrame) -> bytes:
    """将 pandas DataFrame 编码为 Arrow IPC stream 字节"""
    table = pa.Table.from_pandas(df)
    batches = table.to_batches()
    if not batches:
        batches = [pa.RecordBatch.from_pandas(df)]

    sink = pa.BufferOutputStream()
    writer = pa.ipc.new_stream(sink, batches[0].schema)
    try:
        # 遍历写入所有 batch，避免大 DataFrame 数据截断（问题 5）
        for batch in batches:
            writer.write_batch(batch)
    finally:
        writer.close()
    return sink.getvalue().to_pybytes()
