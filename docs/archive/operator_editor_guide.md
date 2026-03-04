# 在线编写 Python 算子功能说明

## 功能概述

在算子管理页面新增"新建算子"功能，允许用户直接在浏览器中编写 Python 算子代码，无需本地编辑器和文件上传。

## 使用方法

### 1. 打开新建算子对话框

在算子管理页面，点击"新建算子"按钮，打开对话框。

### 2. 填写算子信息

**算子名称**（必填）
- 格式：`类别.名称`
- 示例：`explore.chisquare`
- 系统会自动解析类别和名称

**算子描述**（可选）
- 简要描述算子功能
- 示例：卡方独立性检验

**依赖库**（可选）
- 如果需要额外的 Python 库，在此声明
- 格式：`scipy>=1.10.0, scikit-learn>=1.3.0`
- 已预装的库：pandas, numpy, scipy, scikit-learn, statsmodels

### 3. 编写代码

**插入模板**
- 点击"插入模板"按钮，自动插入标准算子模板
- 模板会自动填充算子名称和描述

**插入示例**
- 点击"插入示例"按钮，插入卡方检验示例代码
- 可以基于示例修改实现自己的算子

**手动编写**
- 在代码编辑器中直接编写 Python 代码
- 支持多行编辑，等宽字体显示

### 4. 保存并上传

**保存并上传**
- 将代码保存为 .py 文件并上传到服务器
- 上传后算子处于未激活状态

**保存并激活**
- 保存上传后自动激活算子
- 激活后即可在 SQL 中使用

## 代码规范

### 基本结构

```python
from flowsql import OperatorBase, register_operator
import pandas as pd

@register_operator(
    catelog="类别",
    name="名称",
    description="描述"
)
class MyOperator(OperatorBase):
    """算子类 - 类名可以任意"""

    def work(self, df_in: pd.DataFrame) -> pd.DataFrame:
        """核心处理方法"""
        # 处理逻辑
        return df_out
```

### 关键点

1. **必须继承 `OperatorBase`**
2. **必须实现 `work(self, df_in)` 方法**
3. **使用 `@register_operator` 装饰器注册**
4. **类名可以任意**（如 `ChiSquareOperator`、`MyAnalyzer` 等）

### 获取参数

SQL 的 `WITH` 子句参数通过 `get_config` 方法获取：

```python
def work(self, df_in: pd.DataFrame) -> pd.DataFrame:
    # 获取参数，提供默认值
    threshold = float(self.get_config('threshold', '0.05'))
    method = self.get_config('method', 'pearson')

    # 使用参数处理数据
    # ...

    return df_out
```

SQL 示例：
```sql
SELECT * FROM test.data
USING explore.chisquare
WITH threshold=0.01, method=spearman
```

## 依赖管理

### 预装库

系统已预装以下常用库：
- **pandas** - 数据处理
- **numpy** - 数值计算
- **scipy** - 科学计算
- **scikit-learn** - 机器学习
- **statsmodels** - 统计模型

### 使用额外库

如果需要其他库（如 `matplotlib`、`seaborn`），有两种方式：

**方式 1：在代码中声明**
```python
# DEPENDENCIES: matplotlib>=3.5.0, seaborn>=0.12.0

from flowsql import OperatorBase, register_operator
# ...
```

**方式 2：在依赖库字段填写**
在对话框的"依赖库"输入框中填写：
```
matplotlib>=3.5.0, seaborn>=0.12.0
```

注意：当前版本需要管理员手动安装额外依赖。

### 导入最佳实践

建议在 `work` 方法内部导入，避免启动时加载失败：

```python
def work(self, df_in: pd.DataFrame) -> pd.DataFrame:
    try:
        from scipy.stats import chi2_contingency
        # 使用库
    except ImportError:
        return pd.DataFrame({"error": ["scipy 未安装"]})
```

## 示例算子

### 示例 1：数据过滤

```python
from flowsql import OperatorBase, register_operator
import pandas as pd

@register_operator(
    catelog="filter",
    name="threshold",
    description="按阈值过滤数据"
)
class ThresholdFilter(OperatorBase):

    def work(self, df_in: pd.DataFrame) -> pd.DataFrame:
        # 获取参数
        column = self.get_config('column', df_in.columns[0])
        threshold = float(self.get_config('threshold', '0'))

        # 过滤数据
        df_out = df_in[df_in[column] > threshold]

        return df_out
```

SQL 使用：
```sql
SELECT * FROM test.data
USING filter.threshold
WITH column=bytes_sent, threshold=1000
```

### 示例 2：统计分析

```python
from flowsql import OperatorBase, register_operator
import pandas as pd

@register_operator(
    catelog="stats",
    name="summary",
    description="数据统计摘要"
)
class SummaryStats(OperatorBase):

    def work(self, df_in: pd.DataFrame) -> pd.DataFrame:
        # 计算统计信息
        summary = df_in.describe()

        # 转换为长格式
        result = summary.reset_index()
        result.columns = ['metric'] + list(summary.columns)

        return result
```

SQL 使用：
```sql
SELECT * FROM test.data USING stats.summary
```

### 示例 3：数据转换

```python
from flowsql import OperatorBase, register_operator
import pandas as pd

@register_operator(
    catelog="transform",
    name="normalize",
    description="数据标准化"
)
class Normalizer(OperatorBase):

    def work(self, df_in: pd.DataFrame) -> pd.DataFrame:
        from sklearn.preprocessing import StandardScaler

        # 获取数值列
        numeric_cols = df_in.select_dtypes(include=['number']).columns

        # 标准化
        scaler = StandardScaler()
        df_out = df_in.copy()
        df_out[numeric_cols] = scaler.fit_transform(df_in[numeric_cols])

        return df_out
```

SQL 使用：
```sql
SELECT * FROM test.data USING transform.normalize
```

## 错误处理

建议在算子中添加错误处理：

```python
def work(self, df_in: pd.DataFrame) -> pd.DataFrame:
    try:
        # 处理逻辑
        result = process_data(df_in)
        return result
    except ValueError as e:
        return pd.DataFrame({"error": [f"参数错误: {str(e)}"]})
    except Exception as e:
        return pd.DataFrame({"error": [f"处理失败: {str(e)}"]})
```

## 调试技巧

1. **使用简单数据测试**
   - 先在 SQL 工作台用小数据集测试
   - 确认算子逻辑正确后再处理大数据

2. **返回调试信息**
   ```python
   return pd.DataFrame({
       "debug_info": [f"输入行数: {len(df_in)}, 列数: {df_in.shape[1]}"]
   })
   ```

3. **检查参数**
   ```python
   params = {k: self.get_config(k, 'N/A') for k in ['param1', 'param2']}
   print(f"参数: {params}")  # 日志会输出到服务器控制台
   ```

## 注意事项

1. **性能考虑**
   - 避免在算子中执行耗时操作（如大量网络请求）
   - 大数据集处理时注意内存使用

2. **安全性**
   - 不要在代码中硬编码敏感信息（密码、密钥等）
   - 避免执行系统命令或文件操作

3. **兼容性**
   - 确保代码兼容 Python 3.8+
   - 使用的库版本要与服务器环境匹配

4. **命名规范**
   - 类别名使用小写字母（如 `explore`、`filter`）
   - 算子名使用小写字母和下划线（如 `chi_square`、`threshold_filter`）

## 后续增强

计划中的功能：
- 代码语法高亮（Monaco Editor）
- 代码自动补全
- 在线测试功能
- 算子版本管理
- 依赖自动安装
