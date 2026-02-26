"""卡方分析示例算子"""

import pandas as pd

from flowsql import OperatorBase, OperatorAttribute, register_operator


@register_operator(catelog="explore", name="chisquare", description="卡方独立性检验")
class ChiSquareOperator(OperatorBase):

    def attribute(self) -> OperatorAttribute:
        return OperatorAttribute(
            catelog="explore",
            name="chisquare",
            description="卡方独立性检验",
            position="DATA",
        )

    def work(self, df_in: pd.DataFrame) -> pd.DataFrame:
        """对输入 DataFrame 的前两列执行卡方独立性检验

        输入: 任意 DataFrame（至少两列分类变量）
        输出: DataFrame 包含 statistic, p_value, dof, expected_freq
        """
        try:
            from scipy.stats import chi2_contingency

            if df_in.shape[1] < 2:
                return pd.DataFrame({"error": ["Need at least 2 columns"]})

            # 构建列联表
            col1, col2 = df_in.columns[0], df_in.columns[1]
            contingency = pd.crosstab(df_in[col1], df_in[col2])

            stat, p, dof, expected = chi2_contingency(contingency)

            return pd.DataFrame({
                "statistic": [stat],
                "p_value": [p],
                "dof": [dof],
            })
        except ImportError:
            # scipy 未安装时的降级处理
            return pd.DataFrame({"error": ["scipy not installed"]})
        except Exception as e:
            return pd.DataFrame({"error": [str(e)]})
