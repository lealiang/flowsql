"""算子基类 — 所有 Python 算子继承此类"""

from abc import ABC, abstractmethod
from dataclasses import dataclass, field
from typing import Dict, Any

import pandas as pd


@dataclass
class OperatorAttribute:
    """算子元数据"""
    catelog: str = ""
    name: str = ""
    description: str = ""
    position: str = "DATA"  # "DATA" or "STORAGE"


class OperatorBase(ABC):
    """Python 算子基类"""

    def __init__(self):
        self._config: Dict[str, str] = {}

    @abstractmethod
    def attribute(self) -> OperatorAttribute:
        """返回算子元数据"""
        ...

    @abstractmethod
    def work(self, df_in: pd.DataFrame) -> pd.DataFrame:
        """核心处理方法：接收输入 DataFrame，返回输出 DataFrame"""
        ...

    def configure(self, key: str, value: str):
        """接收配置参数"""
        self._config[key] = value

    def get_config(self, key: str, default: str = "") -> str:
        return self._config.get(key, default)


# 装饰器注册表（模块级）
_registered_operators: Dict[str, type] = {}


def register_operator(catelog: str, name: str, description: str = "", position: str = "DATA"):
    """装饰器：注册算子类

    用法:
        @register_operator(catelog="explore", name="chisquare", description="卡方分析")
        class ChiSquareOperator(OperatorBase):
            ...
    """
    def decorator(cls):
        if not issubclass(cls, OperatorBase):
            raise TypeError(f"{cls.__name__} must inherit from OperatorBase")

        key = f"{catelog}.{name}"
        _registered_operators[key] = cls

        # 注入默认 attribute 实现（如果子类没有覆盖）
        original_attr = getattr(cls, '_decorator_attr', None)
        if original_attr is None:
            cls._decorator_attr = OperatorAttribute(
                catelog=catelog, name=name, description=description, position=position
            )
            # 如果子类的 attribute 是抽象的，提供默认实现
            if getattr(cls.attribute, '__isabstractmethod__', False):
                def _attribute(self):
                    return self._decorator_attr
                cls.attribute = _attribute

        return cls
    return decorator


def get_registered_operators() -> Dict[str, type]:
    """获取通过装饰器注册的所有算子类"""
    return _registered_operators.copy()
