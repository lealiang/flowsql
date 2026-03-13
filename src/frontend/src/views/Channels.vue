<template>
  <div class="channels">
    <h1 class="page-title">通道列表</h1>

    <el-card>
      <template #header>
        <div class="card-header">
          <el-input
            v-model="searchText"
            placeholder="搜索通道名称或类型"
            style="width: 300px"
            clearable
          >
            <template #prefix>
              <el-icon><Search /></el-icon>
            </template>
          </el-input>
          <el-button type="primary" @click="showAddDialog = true">
            <el-icon><Plus /></el-icon> 新增数据库通道
          </el-button>
        </div>
      </template>

      <el-table :data="filteredChannels" style="width: 100%" v-loading="loading">
        <el-table-column prop="name" label="名称" width="200" />
        <el-table-column prop="catelog" label="类别" width="150" />
        <el-table-column prop="type" label="类型" width="150" />
        <el-table-column label="Schema" min-width="300">
          <template #default="scope">
            <el-popover placement="top" :width="400" trigger="hover">
              <template #reference>
                <el-tag>{{ scope.row.schema.length }} 字段</el-tag>
              </template>
              <div class="schema-popup">
                <div v-for="field in scope.row.schema" :key="field.name" class="schema-field">
                  <span class="field-name">{{ field.name }}</span>
                  <span class="field-type">{{ field.type }}</span>
                </div>
              </div>
            </el-popover>
          </template>
        </el-table-column>
        <el-table-column prop="status" label="状态" width="100">
          <template #default="scope">
            <el-tag type="success">{{ scope.row.status }}</el-tag>
          </template>
        </el-table-column>
        <el-table-column label="操作" width="120" v-if="hasDbChannels">
          <template #default="scope">
            <el-button
              v-if="scope.row.catelog && ['sqlite','mysql','clickhouse'].includes(scope.row.catelog)"
              type="danger" size="small" text
              @click="handleRemove(scope.row)"
            >删除</el-button>
          </template>
        </el-table-column>
      </el-table>
    </el-card>

    <!-- 新增数据库通道对话框 -->
    <el-dialog v-model="showAddDialog" title="新增数据库通道" width="500px" @close="resetForm">
      <el-form :model="form" label-width="100px" ref="formRef">
        <el-form-item label="类型" prop="type" required>
          <el-select v-model="form.type" placeholder="选择数据库类型" style="width:100%">
            <el-option label="SQLite" value="sqlite" />
            <el-option label="MySQL" value="mysql" />
            <el-option label="ClickHouse" value="clickhouse" />
          </el-select>
        </el-form-item>
        <el-form-item label="名称" prop="name" required>
          <el-input v-model="form.name" placeholder="通道名称，如 mydb" />
        </el-form-item>

        <!-- SQLite 专属字段 -->
        <template v-if="form.type === 'sqlite'">
          <el-form-item label="文件路径">
            <el-input v-model="form.path" placeholder=":memory: 或 /data/test.db" />
          </el-form-item>
        </template>

        <!-- MySQL / ClickHouse 公共字段 -->
        <template v-if="form.type === 'mysql' || form.type === 'clickhouse'">
          <el-form-item label="主机">
            <el-input v-model="form.host" placeholder="127.0.0.1" />
          </el-form-item>
          <el-form-item label="端口">
            <el-input v-model="form.port" :placeholder="form.type === 'mysql' ? '3306' : '8123'" />
          </el-form-item>
          <el-form-item label="用户名">
            <el-input v-model="form.user" :placeholder="form.type === 'mysql' ? 'root' : 'default'" />
          </el-form-item>
          <el-form-item label="密码">
            <el-input v-model="form.password" type="password" show-password placeholder="留空表示无密码" />
          </el-form-item>
          <el-form-item label="数据库" prop="database" required>
            <el-input v-model="form.database" :placeholder="form.type === 'mysql' ? 'mydb' : 'default'" />
          </el-form-item>
        </template>
      </el-form>
      <template #footer>
        <el-button @click="showAddDialog = false">取消</el-button>
        <el-button type="primary" :loading="submitting" @click="handleAdd">确认添加</el-button>
      </template>
    </el-dialog>
  </div>
</template>

<script setup>
import { ref, computed, onMounted } from 'vue'
import { Search, Plus } from '@element-plus/icons-vue'
import api from '../api'
import { ElMessage, ElMessageBox } from 'element-plus'

const channels = ref([])
const searchText = ref('')
const loading = ref(false)
const showAddDialog = ref(false)
const submitting = ref(false)
const formRef = ref(null)

const form = ref({
  type: 'mysql', name: '', host: '127.0.0.1', port: '',
  user: '', password: '', database: '', path: ''
})

const hasDbChannels = computed(() =>
  channels.value.some(ch => ['sqlite','mysql','clickhouse'].includes(ch.catelog))
)

const filteredChannels = computed(() => {
  if (!searchText.value) return channels.value
  const search = searchText.value.toLowerCase()
  return channels.value.filter(ch =>
    ch.name.toLowerCase().includes(search) ||
    ch.type.toLowerCase().includes(search) ||
    ch.catelog.toLowerCase().includes(search)
  )
})

const loadChannels = async () => {
  loading.value = true
  try {
    const res = await api.getChannels()
    channels.value = res.data.map(ch => ({
      ...ch,
      schema: JSON.parse(ch.schema_json || '[]').map(field => ({
        name: field.name,
        type: getTypeName(field.type)
      }))
    }))
  } catch (error) {
    ElMessage.error('加载通道列表失败: ' + error.message)
  } finally {
    loading.value = false
  }
}

const getTypeName = (typeId) => {
  const typeMap = { 0:'BOOL',1:'INT8',2:'UINT32',3:'UINT64',4:'INT32',5:'INT64',6:'STRING',7:'DOUBLE' }
  return typeMap[typeId] || 'UNKNOWN'
}

// 将表单转为 config_str 格式
const buildConfigStr = () => {
  const f = form.value
  let s = `type=${f.type};name=${f.name}`
  if (f.type === 'sqlite') {
    if (f.path) s += `;path=${f.path}`
  } else {
    if (f.host)     s += `;host=${f.host}`
    if (f.port)     s += `;port=${f.port}`
    if (f.user)     s += `;user=${f.user}`
    if (f.password) s += `;password=${f.password}`
    if (f.database) s += `;database=${f.database}`
  }
  return s
}

const handleAdd = async () => {
  if (!form.value.type || !form.value.name) {
    ElMessage.warning('类型和名称为必填项')
    return
  }
  if ((form.value.type === 'mysql' || form.value.type === 'clickhouse') && !form.value.database) {
    ElMessage.warning('MySQL/ClickHouse 通道必须填写数据库名称')
    return
  }
  submitting.value = true
  try {
    await api.addDbChannel(buildConfigStr())
    ElMessage.success('数据库通道添加成功')
    showAddDialog.value = false
    loadChannels()
  } catch (error) {
    const msg = error.response?.data?.error || error.message
    ElMessage.error('添加失败: ' + msg)
  } finally {
    submitting.value = false
  }
}

const handleRemove = async (row) => {
  try {
    await ElMessageBox.confirm(`确认删除通道 ${row.catelog}.${row.name}？`, '删除确认', { type: 'warning' })
    await api.removeDbChannel(row.catelog, row.name)
    ElMessage.success('删除成功')
    loadChannels()
  } catch (e) {
    if (e !== 'cancel') ElMessage.error('删除失败: ' + (e.response?.data?.error || e.message))
  }
}

const resetForm = () => {
  form.value = { type: 'mysql', name: '', host: '127.0.0.1', port: '', user: '', password: '', database: '', path: '' }
}

onMounted(() => { loadChannels() })
</script>

<style scoped>
.channels { max-width: 1400px; }
.page-title { font-size: 24px; font-weight: 600; margin-bottom: 20px; color: #303133; }
.card-header { display: flex; justify-content: space-between; align-items: center; }
.schema-popup { max-height: 300px; overflow-y: auto; }
.schema-field { display: flex; justify-content: space-between; padding: 5px 0; border-bottom: 1px solid #eee; }
.schema-field:last-child { border-bottom: none; }
.field-name { font-weight: 600; color: #303133; }
.field-type { color: #909399; font-family: monospace; }
</style>
