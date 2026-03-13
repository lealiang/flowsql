import axios from 'axios'

const api = axios.create({
  baseURL: 'http://localhost:8081/api',
  timeout: 30000
})

// 响应拦截器：统一错误处理
api.interceptors.response.use(
  response => response,
  error => {
    console.error('API Error:', error)
    return Promise.reject(error)
  }
)

export default {
  // 健康检查
  health: () => api.get('/health'),

  // 通道
  getChannels: () => api.get('/channels'),

  // 算子
  getOperators: () => api.get('/operators'),
  uploadOperator: (file) => {
    const formData = new FormData()
    formData.append('file', file)
    return api.post('/operators/upload', formData, {
      headers: { 'Content-Type': 'multipart/form-data' }
    })
  },
  activateOperator: (name) => api.post(`/operators/${name}/activate`),
  deactivateOperator: (name) => api.post(`/operators/${name}/deactivate`),

  // 任务
  getTasks: () => api.get('/tasks'),
  createTask: (sql) => api.post('/tasks', { sql }),
  getTaskResult: (id) => api.get(`/tasks/${id}/result`),

  // 数据库通道动态管理（Epic 6）
  listDbChannels: () => api.get('/db-channels'),
  addDbChannel: (config) => api.post('/db-channels/add', { config }),
  removeDbChannel: (type, name) => api.post('/db-channels/remove', { type, name }),
  updateDbChannel: (config) => api.post('/db-channels/update', { config })
}
