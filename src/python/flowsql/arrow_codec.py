"""Arrow IPC 编解码"""

import pyarrow as pa
import polars as pl


def _ensure_arrow_table(df) -> pa.Table:
    """将 DataFrame（Polars 或 Pandas）转为 Arrow Table"""
    if isinstance(df, pa.Table):
        return df
    if isinstance(df, pl.DataFrame):
        return df.to_arrow()
    # pandas DataFrame 或其他兼容类型
    return pa.Table.from_pandas(df)


def decode_arrow_ipc(data: bytes) -> pl.DataFrame:
    """将 Arrow IPC stream 字节解码为 Polars DataFrame"""
    reader = pa.ipc.open_stream(data)
    try:
        table = reader.read_all()
        return pl.from_arrow(table)
    finally:
        reader.close()


def encode_arrow_ipc(df) -> bytes:
    """将 DataFrame（Polars/Pandas）编码为 Arrow IPC stream 字节"""
    table = _ensure_arrow_table(df)
    batches = table.to_batches()
    if not batches:
        batches = [pa.record_batch([], schema=table.schema)]

    sink = pa.BufferOutputStream()
    writer = pa.ipc.new_stream(sink, batches[0].schema)
    try:
        for batch in batches:
            writer.write_batch(batch)
    finally:
        writer.close()
    return sink.getvalue().to_pybytes()


def decode_arrow_ipc_file(path: str) -> pl.DataFrame:
    """从文件 memory_map 读取 Arrow IPC → Polars DataFrame（零拷贝）"""
    mmap = pa.memory_map(path, 'r')
    try:
        reader = pa.ipc.open_stream(mmap)
        try:
            table = reader.read_all()
            return pl.from_arrow(table)
        finally:
            reader.close()
    finally:
        mmap.close()


def encode_arrow_ipc_file(df, path: str):
    """DataFrame（Polars/Pandas）→ Arrow IPC 写入文件"""
    table = _ensure_arrow_table(df)
    batches = table.to_batches()
    if not batches:
        batches = [pa.record_batch([], schema=table.schema)]

    with open(path, 'wb') as f:
        writer = pa.ipc.new_stream(f, batches[0].schema)
        try:
            for batch in batches:
                writer.write_batch(batch)
        finally:
            writer.close()
