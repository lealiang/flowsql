from setuptools import setup, find_packages

setup(
    name="flowsql-worker",
    version="0.1.0",
    packages=find_packages(),
    install_requires=[
        "fastapi>=0.104.0",
        "uvicorn>=0.24.0",
        "pyarrow>=14.0.0",
        "pandas>=2.0.0",
    ],
    entry_points={
        "console_scripts": [
            "flowsql-worker=flowsql.worker:main",
        ],
    },
)
