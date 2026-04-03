# Sprint 13 Ring 并发基线（T30）

日期：2026-04-03  
环境：本地 `build/output/benchmark_stream_ring`（单机）

## 复现命令

```bash
cmake -S src -B build
cmake --build build -j4 --target benchmark_stream_ring
./build/output/benchmark_stream_ring
```

## 原始输出

```text
mode,total,consumed,duplicate,missing,wall_ms,throughput_rows_s,p50_us,p99_us,max_us,cpu_percent
spsc,20000,20000,0,0,322,62111.80,43,963,3400,163.09
spmc,20000,20000,0,0,2775,7207.21,121,685,1340,158.55
mpsc,80000,80000,0,0,4064,19685.04,190025,331866,340826,179.44
mpmc,80000,80000,0,0,321,249221.18,8827,17750,18516,653.72
```

## 结果解读（当前实现）

1. 正确性通过：四种模式 `missing=0` 且 `duplicate=0`。
2. `MPMC` 吞吐最高（本轮场景下约 `249k rows/s`）。
3. `MPSC` 在“4 生产者 + 1 消费者 + 阻塞背压”下出现明显排队延迟，`p99` 偏高，符合单消费者瓶颈预期。
4. `cpu_percent` 为进程 CPU 时间 / 墙钟时间，不是系统级 CPU 使用率，只用于同机同配置相对对比。

## 备注

1. 本基线聚焦 Sprint 13 的 Ring 并发模式补齐（`SPSC/SPMC/MPSC/MPMC`）。
2. `STATELESS` 改造前后对比需单独在相同负载脚本下追加采样，可作为 Sprint 13.5/14 的扩展基线。
