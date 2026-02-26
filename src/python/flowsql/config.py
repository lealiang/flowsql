"""Worker 配置"""

import argparse


class WorkerConfig:
    def __init__(self):
        self.host = "127.0.0.1"
        self.port = 18900
        self.operators_dir = "operators"

    @classmethod
    def from_args(cls):
        parser = argparse.ArgumentParser(description="FlowSQL Python Worker")
        parser.add_argument("--host", default="127.0.0.1")
        parser.add_argument("--port", type=int, default=18900)
        parser.add_argument("--operators-dir", default="operators")
        args = parser.parse_args()

        config = cls()
        config.host = args.host
        config.port = args.port
        config.operators_dir = args.operators_dir
        return config
