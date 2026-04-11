<template>
  <div class="task-runtime">
    <div class="runtime-header">
      <div>
        <h1 class="page-title">任务可视化（执行实例）</h1>
        <p class="subtitle">Task ID: {{ taskId }}</p>
      </div>
      <div class="actions">
        <el-button @click="goBack">返回 SQL 工作台</el-button>
        <el-button :icon="Refresh" @click="refreshNow" :loading="loading">刷新</el-button>
      </div>
    </div>

    <el-card class="meta-card" v-loading="loading">
      <template #header>
        <div class="card-title">运行实例概览</div>
      </template>
      <div class="meta-grid" v-if="graph">
        <div class="meta-item">
          <span class="k">状态</span>
          <el-tag :type="statusTag(meta.status)">{{ meta.status }}</el-tag>
        </div>
        <div class="meta-item">
          <span class="k">任务类型</span>
          <el-tag type="info">{{ meta.taskKind }}</el-tag>
        </div>
        <div class="meta-item">
          <span class="k">运行类型</span>
          <el-tag type="warning">{{ meta.runtimeKind }}</el-tag>
        </div>
        <div class="meta-item">
          <span class="k">节点数</span>
          <span class="v">{{ canvas.nodes.length }}</span>
        </div>
        <div class="meta-item">
          <span class="k">边数</span>
          <span class="v">{{ canvas.edges.length }}</span>
        </div>
        <div class="meta-item">
          <span class="k">快照时间</span>
          <span class="v">{{ snapshotTimeText }}</span>
        </div>
      </div>
      <el-empty v-else description="暂无运行实例数据" />
    </el-card>

    <el-card class="sql-card" v-loading="loading">
      <template #header>
        <div class="card-title">SQL 对照</div>
      </template>
      <div v-if="runtimeSqls.length > 0" class="sql-list">
        <div v-for="(sql, idx) in runtimeSqls" :key="idx" class="sql-item">
          <div class="sql-item-title">SQL {{ idx + 1 }}</div>
          <pre class="sql-code">{{ sql }}</pre>
        </div>
      </div>
      <el-empty v-else description="暂无 SQL 文本" />
    </el-card>

    <el-card class="graph-card" v-loading="loading">
      <template #header>
        <div class="graph-title-row">
          <span class="card-title">任务 DAG（单画布连线）</span>
          <div class="legend">
            <span class="legend-item"><i class="swatch on-data"></i>on_data</span>
            <span class="legend-item"><i class="swatch on-start"></i>on_start</span>
            <span class="legend-item"><i class="swatch on-finish"></i>on_finish</span>
          </div>
        </div>
      </template>

      <el-empty v-if="canvas.nodes.length === 0" description="未获取到可视化节点" />

      <div v-else class="canvas-wrap" :style="{ background: canvasBackground }">
        <svg :width="canvas.width" :height="canvas.height" :viewBox="`0 0 ${canvas.width} ${canvas.height}`">
          <defs>
            <marker id="arrow" markerWidth="8" markerHeight="8" refX="7" refY="4" orient="auto" markerUnits="strokeWidth">
              <path d="M 0 0 L 8 4 L 0 8 z" :fill="edgeArrowColor" />
            </marker>
          </defs>

          <g class="edge-layer">
            <g v-for="edge in canvas.edges" :key="edge.id">
              <path
                :d="edge.path"
                :stroke="edgeColor(edge)"
                :stroke-dasharray="edgeDash(edge)"
                :stroke-width="edgeWidth(edge)"
                fill="none"
                marker-end="url(#arrow)"
              />
              <text
                v-if="edge.label"
                :x="edge.labelX"
                :y="edge.labelY"
                class="edge-label"
              >
                {{ edge.label }}
              </text>
            </g>
          </g>

          <g class="node-layer">
            <g
              v-for="node in canvas.nodes"
              :key="node.id"
              :transform="`translate(${node.x}, ${node.y})`"
            >
              <rect
                :width="NODE_W"
                :height="NODE_H"
                :rx="10"
                :fill="nodeFill(node)"
                :stroke="nodeStroke(node)"
                stroke-width="1.5"
              />
              <text x="10" y="16" class="node-kind">{{ node.kind || 'unknown' }}</text>
              <text x="10" y="30" class="node-name">{{ truncate(node.name || node.id, 18) }}</text>
              <text x="10" y="43" class="node-id">{{ truncate(node.id, 22) }}</text>
              <text x="10" y="57" class="node-meta">
                sql_index={{ normalizeSqlIndex(node.sql_index) }}
                <tspan v-if="node.phase"> · {{ node.phase }}</tspan>
              </text>
              <text x="10" y="71" class="node-metrics">
                p={{ Number(node.processed_rows || 0) }} / o={{ Number(node.output_rows || 0) }}
              </text>
              <text v-if="node.error_message" x="10" y="85" class="node-error">
                {{ truncate(node.error_message, 18) }}
              </text>
            </g>
          </g>
        </svg>
      </div>
    </el-card>
  </div>
</template>

<script setup>
import { computed, onMounted, onUnmounted, ref } from 'vue'
import { useRoute, useRouter } from 'vue-router'
import { Refresh } from '@element-plus/icons-vue'
import { ElMessage } from 'element-plus'
import api from '../api'

const NODE_W = 180
const NODE_H = 96
const PAD_X = 20
const PAD_Y = 16
const H_GAP = 60
const V_GAP = 16

const route = useRoute()
const router = useRouter()
const taskId = computed(() => String(route.params.taskId || ''))
const isDarkTheme = ref(document.documentElement.classList.contains('dark'))

const loading = ref(false)
const graph = ref(null)
const cursor = ref(0)
const events = ref([])
let pollTimer = null
let themeObserver = null

const isTerminal = (status) => {
  const s = String(status || '').toLowerCase()
  return ['completed', 'failed', 'stopped', 'cancelled', 'timeout'].includes(s)
}

const statusTag = (status) => {
  const s = String(status || '').toLowerCase()
  if (s === 'completed' || s === 'stopped' || s === 'done') return 'success'
  if (s === 'running' || s === 'pending' || s === 'created' || s === 'preparing' || s === 'submitted' || s === 'active' || s === 'idle') return 'warning'
  return 'danger'
}

const normalizeSqlIndex = (value) => {
  if (Number.isInteger(value)) return value
  if (typeof value === 'number' && Number.isFinite(value)) return Math.trunc(value)
  return -1
}

const truncate = (text, max = 24) => {
  const s = String(text || '')
  if (s.length <= max) return s
  return s.slice(0, max - 1) + '…'
}

const meta = computed(() => ({
  status: graph.value?.status || 'unknown',
  taskKind: graph.value?.task_kind || 'unknown',
  runtimeKind: graph.value?.runtime_kind || 'unknown',
  snapshotTimeMs: Number(graph.value?.snapshot_time_ms || 0)
}))

const snapshotTimeText = computed(() => {
  const ts = meta.value.snapshotTimeMs
  if (!Number.isFinite(ts) || ts <= 0) return '-'
  return new Date(ts).toLocaleString()
})

const splitSqlText = (text) => {
  const parts = String(text || '')
    .split(';')
    .map((s) => s.trim())
    .filter((s) => s.length > 0)
  return parts
}

const runtimeSqls = computed(() => {
  const fromArray = Array.isArray(graph.value?.sqls)
    ? graph.value.sqls.map((s) => String(s || '').trim()).filter((s) => s.length > 0)
    : []
  if (fromArray.length > 0) return fromArray
  const raw = String(graph.value?.sql_text || '').trim()
  if (!raw) return []
  return splitSqlText(raw)
})

const canvasBackground = computed(() => {
  if (isDarkTheme.value) {
    return 'radial-gradient(circle at 16% 18%, #1e293b 0%, #0f172a 48%, #0b1220 100%)'
  }
  return 'radial-gradient(circle at 20% 20%, #f8fbff 0%, #ffffff 48%, #f8fafc 100%)'
})

const edgeArrowColor = computed(() => (isDarkTheme.value ? '#94a3b8' : '#64748b'))

const normalizedNodes = computed(() => {
  const list = Array.isArray(graph.value?.nodes) ? graph.value.nodes.slice() : []
  if (list.length > 0) {
    return list.sort((a, b) => {
      const ai = normalizeSqlIndex(a?.sql_index)
      const bi = normalizeSqlIndex(b?.sql_index)
      if (ai !== bi) return ai - bi
      return String(a?.id || '').localeCompare(String(b?.id || ''))
    })
  }
  if (!graph.value) return []
  return [{
    id: String(taskId.value),
    kind: 'operator',
    name: graph.value?.task_kind === 'batch' ? 'batch_worker' : 'operator',
    sql_index: 0,
    status: graph.value?.status || 'unknown',
    phase: '',
    processed_rows: 0,
    output_rows: 0,
    error_message: graph.value?.error_message || ''
  }]
})

const normalizedEdges = computed(() => {
  const list = Array.isArray(graph.value?.edges) ? graph.value.edges.slice() : []
  return list.sort((a, b) => String(a?.id || '').localeCompare(String(b?.id || '')))
})

const canvas = computed(() => {
  const nodes = normalizedNodes.value
  const edges = normalizedEdges.value
  if (nodes.length === 0) {
    return { width: 800, height: 420, nodes: [], edges: [] }
  }

  const nodeMap = new Map()
  nodes.forEach((n) => nodeMap.set(String(n.id), n))

  const indegree = new Map()
  const adj = new Map()
  nodes.forEach((n) => {
    const id = String(n.id)
    indegree.set(id, 0)
    adj.set(id, [])
  })

  const validEdges = []
  edges.forEach((e) => {
    const from = String(e?.from || '')
    const to = String(e?.to || '')
    if (!nodeMap.has(from) || !nodeMap.has(to)) return
    validEdges.push({ ...e, from, to })
    adj.get(from).push(to)
    indegree.set(to, (indegree.get(to) || 0) + 1)
  })

  const level = new Map()
  const queue = [...indegree.entries()]
    .filter(([, v]) => v === 0)
    .map(([id]) => id)
    .sort((a, b) => a.localeCompare(b))

  const indegreeWork = new Map(indegree)
  const visited = new Set()
  queue.forEach((id) => level.set(id, 0))

  while (queue.length > 0) {
    const cur = queue.shift()
    visited.add(cur)
    const curLevel = level.get(cur) || 0
    const nexts = adj.get(cur) || []
    for (const to of nexts) {
      const candidate = curLevel + 1
      level.set(to, Math.max(level.get(to) || 0, candidate))
      indegreeWork.set(to, (indegreeWork.get(to) || 1) - 1)
      if (indegreeWork.get(to) === 0) {
        queue.push(to)
      }
    }
    queue.sort((a, b) => a.localeCompare(b))
  }

  nodes.forEach((n) => {
    const id = String(n.id)
    if (!level.has(id)) level.set(id, 0)
  })

  const buckets = new Map()
  nodes.forEach((n) => {
    const id = String(n.id)
    const lv = level.get(id) || 0
    if (!buckets.has(lv)) buckets.set(lv, [])
    buckets.get(lv).push(n)
  })

  const cols = [...buckets.keys()].sort((a, b) => a - b)
  cols.forEach((c) => {
    buckets.get(c).sort((a, b) => String(a.id).localeCompare(String(b.id)))
  })

  let maxRows = 0
  cols.forEach((c) => {
    maxRows = Math.max(maxRows, buckets.get(c).length)
  })
  const width = Math.max(640, PAD_X * 2 + cols.length * NODE_W + Math.max(0, cols.length - 1) * H_GAP)
  const height = Math.max(380, PAD_Y * 2 + maxRows * NODE_H + Math.max(0, maxRows - 1) * V_GAP)

  const pos = new Map()
  cols.forEach((c, colIndex) => {
    const colNodes = buckets.get(c)
    colNodes.forEach((n, rowIndex) => {
      const x = PAD_X + colIndex * (NODE_W + H_GAP)
      const y = PAD_Y + rowIndex * (NODE_H + V_GAP)
      pos.set(String(n.id), { x, y })
    })
  })

  const renderNodes = nodes.map((n) => {
    const p = pos.get(String(n.id)) || { x: PAD_X, y: PAD_Y }
    return { ...n, x: p.x, y: p.y }
  })

  const renderEdges = validEdges.map((e) => {
    const fromPos = pos.get(e.from)
    const toPos = pos.get(e.to)
    if (!fromPos || !toPos) {
      return {
        ...e,
        path: '',
        labelX: 0,
        labelY: 0,
        label: String(e.trigger || 'on_data')
      }
    }

    const sx = fromPos.x + NODE_W
    const sy = fromPos.y + NODE_H / 2
    const tx = toPos.x
    const ty = toPos.y + NODE_H / 2
    const delta = Math.max(24, Math.abs(tx - sx) * 0.25)
    const c1x = sx + delta
    const c2x = tx - delta
    const d = `M ${sx} ${sy} C ${c1x} ${sy}, ${c2x} ${ty}, ${tx} ${ty}`

    return {
      ...e,
      path: d,
      labelX: (sx + tx) / 2,
      labelY: (sy + ty) / 2 - 6,
      label: String(e.trigger || 'on_data')
    }
  })

  return { width, height, nodes: renderNodes, edges: renderEdges }
})

const nodeFill = (node) => {
  if (isDarkTheme.value) {
    const kind = String(node?.kind || '').toLowerCase()
    if (kind === 'channel') return '#0b3e49'
    if (kind === 'operator') return '#3a2554'
    if (kind === 'control') return '#4a3415'
    return '#263244'
  }
  const kind = String(node?.kind || '').toLowerCase()
  if (kind === 'channel') return '#ecfeff'
  if (kind === 'operator') return '#eff6ff'
  if (kind === 'control') return '#fffbeb'
  return '#f8fafc'
}

const nodeStroke = (node) => {
  const status = String(node?.status || '').toLowerCase()
  if (status === 'completed' || status === 'stopped' || status === 'done') return '#16a34a'
  if (status === 'running' || status === 'pending' || status === 'active' || status === 'created' || status === 'preparing') return '#f59e0b'
  return '#ef4444'
}

const edgeColor = (edge) => {
  const status = String(edge?.status || '').toLowerCase()
  if (status === 'blocked') return '#ef4444'
  const trigger = String(edge?.trigger || 'on_data').toLowerCase()
  if (trigger === 'on_start') return '#64748b'
  if (trigger === 'on_finish') return '#16a34a'
  return '#2563eb'
}

const edgeDash = (edge) => {
  const trigger = String(edge?.trigger || 'on_data').toLowerCase()
  if (trigger === 'on_start') return '6 5'
  if (trigger === 'on_finish') return '3 5'
  return ''
}

const edgeWidth = (edge) => {
  const status = String(edge?.status || '').toLowerCase()
  if (status === 'active') return 2.6
  return 1.8
}

const stopPolling = () => {
  if (pollTimer) {
    clearInterval(pollTimer)
    pollTimer = null
  }
}

const loadRuntimeGraph = async ({ silent = false, resetCursor = false } = {}) => {
  if (!taskId.value) return
  if (resetCursor) {
    cursor.value = 0
    events.value = []
  }
  loading.value = true
  try {
    const res = await api.getTaskRuntimeGraph(taskId.value, cursor.value, true)
    const payload = res?.data || {}
    graph.value = payload

    const batch = Array.isArray(payload.events) ? payload.events : []
    if (batch.length > 0) {
      events.value = [...events.value, ...batch].slice(-200)
    }

    const nextCursor = payload?.next_cursor
    if (Number.isFinite(nextCursor) && nextCursor >= 0) {
      cursor.value = Number(nextCursor)
    }
  } catch (error) {
    if (!silent) {
      const msg = error?.response?.data?.error || error?.message || '未知错误'
      ElMessage.error('加载运行实例失败: ' + msg)
    }
  } finally {
    loading.value = false
  }
}

const refreshNow = async () => {
  await loadRuntimeGraph({ resetCursor: true })
}

const startPolling = () => {
  if (pollTimer) return
  pollTimer = setInterval(async () => {
    await loadRuntimeGraph({ silent: true })
    if (isTerminal(graph.value?.status)) {
      stopPolling()
    }
  }, 2000)
}

const goBack = () => {
  router.push('/tasks')
}

onMounted(async () => {
  const syncTheme = () => {
    isDarkTheme.value = document.documentElement.classList.contains('dark')
  }
  syncTheme()
  themeObserver = new MutationObserver(syncTheme)
  themeObserver.observe(document.documentElement, {
    attributes: true,
    attributeFilter: ['class']
  })

  await loadRuntimeGraph({ resetCursor: true })
  if (!isTerminal(graph.value?.status)) {
    startPolling()
  }
})

onUnmounted(() => {
  stopPolling()
  if (themeObserver) {
    themeObserver.disconnect()
    themeObserver = null
  }
})
</script>

<style scoped>
.task-runtime {
  display: flex;
  flex-direction: column;
  gap: 16px;
}

.runtime-header {
  display: flex;
  align-items: flex-start;
  justify-content: space-between;
  gap: 12px;
}

.page-title {
  margin: 0;
  font-size: 22px;
  font-weight: 700;
}

.subtitle {
  margin: 4px 0 0;
  color: var(--text-secondary);
}

.actions {
  display: flex;
  gap: 8px;
}

.card-title {
  font-weight: 600;
}

.graph-title-row {
  display: flex;
  justify-content: space-between;
  align-items: center;
  gap: 12px;
}

.legend {
  display: flex;
  align-items: center;
  gap: 10px;
  flex-wrap: wrap;
}

.legend-item {
  display: inline-flex;
  align-items: center;
  gap: 6px;
  color: var(--text-secondary);
  font-size: 12px;
}

.swatch {
  width: 20px;
  height: 2px;
  display: inline-block;
}

.swatch.on-data {
  background: #2563eb;
}

.swatch.on-start {
  background: #64748b;
  border-top: 1px dashed #64748b;
  height: 0;
}

.swatch.on-finish {
  background: #16a34a;
  border-top: 1px dashed #16a34a;
  height: 0;
}

.meta-grid {
  display: grid;
  grid-template-columns: repeat(auto-fill, minmax(180px, 1fr));
  gap: 10px;
}

.meta-item {
  display: flex;
  align-items: center;
  gap: 8px;
  min-height: 32px;
}

.meta-item .k {
  color: var(--text-secondary);
  font-size: 13px;
}

.meta-item .v {
  font-weight: 600;
}

.sql-list {
  display: flex;
  flex-direction: column;
  gap: 10px;
}

.sql-item {
  border: 1px solid var(--border-color);
  border-radius: 8px;
  background: var(--bg-card);
  overflow: hidden;
}

.sql-item-title {
  padding: 6px 10px;
  font-size: 12px;
  color: var(--text-secondary);
  border-bottom: 1px solid var(--border-color);
}

.sql-code {
  margin: 0;
  padding: 10px;
  font-size: 12px;
  line-height: 1.45;
  color: var(--text-primary);
  font-family: ui-monospace, SFMono-Regular, Menlo, Monaco, Consolas, "Liberation Mono", "Courier New", monospace;
  white-space: pre-wrap;
  word-break: break-word;
  background: transparent;
}

.canvas-wrap {
  overflow: auto;
  border: 1px solid var(--border-color);
  border-radius: 10px;
}

svg {
  display: block;
}

.node-kind {
  font-size: 10px;
  font-weight: 700;
  fill: var(--text-secondary);
}

.node-name {
  font-size: 12px;
  font-weight: 700;
  fill: var(--text-primary);
}

.node-id {
  font-size: 10px;
  fill: var(--text-secondary);
}

.node-meta,
.node-metrics {
  font-size: 10px;
  fill: var(--text-primary);
}

.node-error {
  font-size: 10px;
  fill: #b91c1c;
}

.edge-label {
  font-size: 10px;
  fill: var(--text-secondary);
  text-anchor: middle;
}

@media (max-width: 900px) {
  .runtime-header {
    flex-direction: column;
    align-items: stretch;
  }

  .actions {
    justify-content: flex-start;
  }

  .graph-title-row {
    flex-direction: column;
    align-items: flex-start;
  }
}
</style>
