#!/bin/bash
# FlowSQL 前端功能测试脚本

set -e

BASE_URL="http://localhost:8081"
API_URL="$BASE_URL/api"

echo "=========================================="
echo "  FlowSQL 前端功能测试"
echo "=========================================="
echo ""

# 1. 健康检查
echo "[1/6] 测试健康检查..."
HEALTH=$(curl -s "$API_URL/health")
if echo "$HEALTH" | grep -q '"status":"ok"'; then
    echo "✓ 健康检查通过"
else
    echo "✗ 健康检查失败"
    exit 1
fi
echo ""

# 2. 测试通道列表
echo "[2/6] 测试通道列表..."
CHANNELS=$(curl -s "$API_URL/channels")
CHANNEL_COUNT=$(echo "$CHANNELS" | grep -o '"name"' | wc -l)
if [ "$CHANNEL_COUNT" -ge 1 ]; then
    echo "✓ 通道列表获取成功 ($CHANNEL_COUNT 个通道)"
else
    echo "✗ 通道列表获取失败"
    exit 1
fi
echo ""

# 3. 测试算子列表
echo "[3/6] 测试算子列表..."
OPERATORS=$(curl -s "$API_URL/operators")
OPERATOR_COUNT=$(echo "$OPERATORS" | grep -o '"name"' | wc -l)
if [ "$OPERATOR_COUNT" -ge 1 ]; then
    echo "✓ 算子列表获取成功 ($OPERATOR_COUNT 个算子)"
else
    echo "✗ 算子列表获取失败"
    exit 1
fi
echo ""

# 4. 测试任务列表
echo "[4/6] 测试任务列表..."
TASKS=$(curl -s "$API_URL/tasks")
echo "✓ 任务列表获取成功"
echo ""

# 5. 提交 SQL 任务
echo "[5/6] 提交 SQL 任务..."
SQL='{"sql":"SELECT * FROM test.data USING example.passthrough"}'
TASK_RESULT=$(curl -s -X POST "$API_URL/tasks" -H "Content-Type: application/json" -d "$SQL")
TASK_ID=$(echo "$TASK_RESULT" | grep -o '"task_id":[0-9]*' | grep -o '[0-9]*')
if [ -n "$TASK_ID" ]; then
    echo "✓ 任务提交成功 (ID: $TASK_ID)"
else
    echo "✗ 任务提交失败"
    exit 1
fi
echo ""

# 6. 获取任务结果
echo "[6/6] 获取任务结果..."
RESULT=$(curl -s "$API_URL/tasks/$TASK_ID/result")
if echo "$RESULT" | grep -q '"status":"completed"'; then
    ROW_COUNT=$(echo "$RESULT" | grep -o '\[' | wc -l)
    echo "✓ 任务执行成功"
    echo "  结果包含数据行"
else
    echo "✗ 任务执行失败"
    echo "  结果: $RESULT"
    exit 1
fi
echo ""

# 7. 测试前端页面
echo "[7/7] 测试前端页面..."
HOMEPAGE=$(curl -s "$BASE_URL/")
if echo "$HOMEPAGE" | grep -q "FlowSQL"; then
    echo "✓ 前端页面加载成功"
else
    echo "✗ 前端页面加载失败"
    exit 1
fi
echo ""

echo "=========================================="
echo "  所有测试通过！"
echo "=========================================="
echo ""
echo "访问前端：$BASE_URL"
echo ""
