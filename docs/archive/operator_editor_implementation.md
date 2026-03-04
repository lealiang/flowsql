# 在线编写 Python 算子功能 - 实现总结

## 实现内容

为算子管理页面添加了在线编写 Python 算子的功能，用户无需本地编辑器即可直接在浏览器中创建和上传算子。

## 功能特性

### 1. 新建算子对话框

- **算子名称输入**：自动解析 `类别.名称` 格式
- **算子描述**：可选的功能描述
- **依赖库声明**：支持声明额外的 Python 库依赖
- **代码编辑器**：20 行高度的 textarea，等宽字体显示
- **模板插入**：一键插入标准算子模板
- **示例插入**：一键插入卡方检验示例代码

### 2. 代码模板

提供两种模板：

**标准模板**
- 基础的算子结构
- 包含注释说明
- 自动填充用户输入的名称和描述

**示例模板**
- 完整的卡方检验实现
- 包含参数获取、错误处理
- 可直接运行的示例代码

### 3. 保存选项

- **保存并上传**：上传后处于未激活状态
- **保存并激活**：上传后自动激活，可立即使用

### 4. 依赖管理

更新了 `requirements.txt`，预装常用数据科学库：
- numpy >= 1.24.0
- scipy >= 1.10.0
- scikit-learn >= 1.3.0
- statsmodels >= 0.14.0

## 技术实现

### 前端实现

**对话框组件**
- 使用 `el-dialog` 创建模态对话框
- 宽度 900px，适合代码编辑
- 禁用点击遮罩关闭，避免误操作

**表单验证**
- 算子名称必填，格式验证
- 代码内容必填
- 实时解析类别和名称

**代码转文件**
```javascript
// 将代码文本转为 Blob
const blob = new Blob([finalCode], { type: 'text/plain' })
const file = new File([blob], fileName, { type: 'text/plain' })

// 使用现有的上传 API
await api.uploadOperator(file)
```

**依赖注释**
如果用户填写了依赖库，自动添加到代码开头：
```python
# DEPENDENCIES: scipy>=1.10.0, scikit-learn>=1.3.0

from flowsql import OperatorBase, register_operator
# ...
```

### 后端支持

无需修改后端代码，完全复用现有的 `/api/operators/upload` 接口。

## 界面预览

```
┌─────────────────────────────────────────────────────┐
│ 算子管理                                             │
├─────────────────────────────────────────────────────┤
│ 算子列表              [新建算子] [上传文件]          │
│ ┌─────────────────────────────────────────────────┐ │
│ │ 名称 │ 类别 │ 位置 │ 状态 │ 操作              │ │
│ └─────────────────────────────────────────────────┘ │
└─────────────────────────────────────────────────────┘

点击"新建算子"后：
┌─────────────────────────────────────────────────────┐
│ 新建 Python 算子                              [×]   │
├─────────────────────────────────────────────────────┤
│ 算子名称: [explore.chisquare_______________] [?]    │
│ 类别: explore  名称: chisquare                       │
│                                                      │
│ 算子描述: [卡方独立性检验_____________________]      │
│                                                      │
│ 依赖库: [scipy>=1.10.0____________________]         │
│ 已预装: pandas, numpy, scipy, scikit-learn...       │
│                                                      │
│ Python 代码:        [插入模板] [插入示例]           │
│ ┌──────────────────────────────────────────────┐   │
│ │ from flowsql import OperatorBase, ...        │   │
│ │                                              │   │
│ │ @register_operator(                          │   │
│ │     catelog="explore",                       │   │
│ │     name="chisquare",                        │   │
│ │     description="卡方检验"                    │   │
│ │ )                                            │   │
│ │ class ChiSquareOperator(OperatorBase):       │   │
│ │     def work(self, df_in):                   │   │
│ │         # 处理逻辑...                         │   │
│ │         return df_in                         │   │
│ │                                              │   │
│ └──────────────────────────────────────────────┘   │
│                                                      │
│          [取消]  [保存并上传]  [保存并激活]         │
└─────────────────────────────────────────────────────┘
```

## 使用流程

1. 点击"新建算子"按钮
2. 输入算子名称（如 `explore.chisquare`）
3. 填写算子描述（可选）
4. 声明依赖库（可选）
5. 点击"插入模板"或"插入示例"
6. 修改代码实现自己的逻辑
7. 点击"保存并激活"
8. 在 SQL 工作台中使用新算子

## 示例：创建过滤算子

**步骤 1：填写信息**
- 算子名称：`filter.threshold`
- 算子描述：按阈值过滤数据

**步骤 2：编写代码**
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
        column = self.get_config('column', df_in.columns[0])
        threshold = float(self.get_config('threshold', '0'))
        return df_in[df_in[column] > threshold]
```

**步骤 3：保存并激活**

**步骤 4：在 SQL 中使用**
```sql
SELECT * FROM test.data
USING filter.threshold
WITH column=bytes_sent, threshold=1000
```

## 文件清单

### 修改的文件
- `src/web-ui/src/views/Operators.vue` - 新增在线编写功能
- `src/python/requirements.txt` - 添加常用数据科学库

### 新建的文件
- `docs/operator_editor_guide.md` - 功能使用指南

## 优势

1. **降低门槛**：无需本地 Python 环境和编辑器
2. **快速开发**：模板和示例加速开发
3. **即时验证**：保存后立即可在 SQL 中测试
4. **统一管理**：所有算子在同一界面管理

## 后续优化建议

### 短期优化
1. **代码语法高亮**：集成 Monaco Editor
2. **代码格式化**：添加格式化按钮
3. **代码验证**：上传前检查基本语法

### 中期优化
1. **在线测试**：提供测试数据和预览结果
2. **代码补全**：提供 pandas/numpy API 补全
3. **错误提示**：显示详细的错误信息

### 长期优化
1. **版本管理**：保存算子的历史版本
2. **依赖自动安装**：检测并自动安装缺失的库
3. **算子市场**：分享和下载社区算子

## 总结

成功实现了在线编写 Python 算子功能，用户可以直接在浏览器中创建、编辑和上传算子，大大简化了算子开发流程。配合预装的常用数据科学库，用户可以快速实现各种数据处理和分析功能。
