"""Arrow IPC 编解码"""

import io
import pyarrow as pa
import pandas as pd


def decode_arrow_ipc(data: bytes) -> pd.DataFrame:
    """将 Arrow IPC stream 字节解码为 pandas DataFrame"""
    reader = pa.ipc.open_stream(data)
    table = reader.read_all()
    return table.to_pandas()


def encode_arrow_ipc(df: pd.DataFrame) -> bytes:
    """将 pandas DataFrame 编码为 Arrow IPC stream 字节"""
    table = pa.Table.from_pandas(df)
    batch = table.to_batches()[0] if table.num_rows > 0 else pa.RecordBatch.from_pandas(df)

    sink = pa.BufferOutputStream()
    writer = pa.ipc.new_stream(sink, batch.schema)
    writer.write_batch(batch)
    writer.close()
    return sink.getvalue().to_pybytes()
