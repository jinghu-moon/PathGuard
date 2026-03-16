# 规则引擎性能与内存报告（基准）

本报告汇总了“全量基准”和“仅匹配基准（更接近真实运行）”的结果，便于理解 `mem_peak` 的差异。

## 1. 全量基准（默认：所有基准项）

### 1.1 真实环境（默认配置，`--benchmark_min_time=0.2s`）
- 5 次均值：
  - `elapsed_ms`：6122.99
  - `cpu_total_sec`：6.363
  - `cpu_avg_pct`：5.20%
  - `mem_avg_mb`：17.45
  - `mem_peak_mb`：132.76
- 示例输出（run5）：`build/bench_run_opt_real_5.txt`

> 说明：该模式包含规则构建、内存估算、缓存压力等基准，会放大进程 WorkingSet 峰值。

### 1.2 稳定采样（固定单核亲和性 + 全核占比）
- 5 次均值：
  - `elapsed_ms`：5909.16
  - `cpu_total_sec`：5.272
  - `cpu_avg_pct`：4.47%
  - `mem_avg_mb`：22.54
  - `mem_peak_mb`：141.69
- 示例输出（run5）：`build/bench_run_cpp20_lvl2_vec_aff_all_5.txt`

> 说明：固定亲和性使 CPU 更稳定，但峰值内存仍会被“全量基准项”放大。

## 2. 仅匹配基准（更接近真实模块）

### 2.1 真实环境（`--benchmark_filter=BM_Match`）
- 5 次均值：
  - `elapsed_ms`：2761.36
  - `cpu_total_sec`：2.550
  - `cpu_avg_pct`：4.62%
  - `mem_avg_mb`：11.42
  - `mem_peak_mb`：12.32
  - `priv_avg_mb`：2.26
  - `priv_peak_mb`：3.10
- 示例输出（run5）：`build/bench_run_real_match_5.txt`

> 说明：`priv_*` 是进程私有内存，更接近模块真实常驻占用。

## 3. 关键结论

1. “全量基准”中的 `mem_peak` 主要由**规则构建、路径批量生成、内存估算**拉高，不代表模块真实常驻。
2. “仅匹配基准”显示：私有内存峰值约 **3MB**，更接近真实运行环境。
3. 热路径匹配性能（`BM_MatchBatch10k`）已经稳定达到 **~6.7M/s** 量级。

## 4. 对比表（均值）

| 模式 | benchmark | elapsed_ms | cpu_total_sec | cpu_avg_pct | mem_avg_mb | mem_peak_mb | priv_avg_mb | priv_peak_mb |
|---|---|---:|---:|---:|---:|---:|---:|---:|
| 全量基准 | `--benchmark_min_time=0.2s` | 6122.99 | 6.363 | 5.20% | 17.45 | 132.76 | - | - |
| 全量基准（固定单核） | `--benchmark_min_time=0.2s` | 5909.16 | 5.272 | 4.47% | 22.54 | 141.69 | - | - |
| 仅匹配基准 | `--benchmark_filter=BM_Match` | 2761.36 | 2.550 | 4.62% | 11.42 | 12.32 | 2.26 | 3.10 |

## 5. 复现实验命令（PowerShell）

### 全量基准
```
"D:/100_Projects/110_Daily/folder-manager/build/Release/fm_rule_engine_bench.exe" --benchmark_min_time=0.2s
```

### 仅匹配基准
```
"D:/100_Projects/110_Daily/folder-manager/build/Release/fm_rule_engine_bench.exe" --benchmark_min_time=0.2s --benchmark_filter=BM_Match
```
