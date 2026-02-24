# OBS C SDK Benchmark Tool (obs_c_bench)

`obs_c_bench` 是一款专为华为云对象存储服务 (OBS) 打造的工业级、高性能 C 语言压测工具。它基于官方 OBS C SDK 开发，采用了极致的无锁并发架构，旨在帮助开发者、架构师和测试工程师准确评估云存储后端的极限 TPS、吞吐带宽以及长尾时延表现。

## 🌟 核心特性 (Key Features)

* **极致的并发性能 (Lock-Free Architecture)**
* Worker 线程执行请求及本地统计数据收集时**全程无锁**，榨干压测机每一滴 CPU 性能。
* 独立的旁路监控线程（Monitor Thread）每 3 秒无锁采集全局状态，实时输出累计 TPS、带宽与成功率，对发流性能 **0 干扰**。


* **确定性伪随机打散 (LCG Hash Naming)**
* 内置标准 LCG (Linear Congruential Generator) 算法。开启 `ObjNamePatternHash=true` 后，可在对象名前缀生成均匀离散的 Hash 值，**彻底消除云存储底层分片的热点瓶颈**。
* 算法具备强确定性，确保先执行 PUT 压测后，再次执行 GET 压测能够 100% 精确命中已上传的对象。


* **零拷贝异构数据校验 (Zero-Copy Validation)**
* 支持 `EnableDataValidation=true`。工具在内存中预分配 1MB 的确定性特征环形缓冲区（Pattern Buffer）。
* 下载过程在网络回调层实时计算绝对偏移量，进行异构比对。即使并发 Range 下载，也能在极低 CPU 消耗下完成严格的**数据一致性校验**，精准捕获静默错误 (DataConsistencyError)。


* **智能多租户桶路由 (Smart Bucket Routing)**
* 支持 `users.dat` 批量加载多账户。
* 动态桶名拼接策略：自动按照 `{ak_lowercase}.{BucketNamePrefix}` 的格式将流量路由至各账户的专属桶，或通过 `BucketNameFixed` 强制打向固定桶。


* **防爆内存的海量流水落盘 (Log Rotation)**
* 开启 `EnableDetailLog=true` 后，支持请求级明细流水落盘。
* 工具自动按任务时间戳创建独立隔离目录（如 `logs/task_20260224_120000/`）。
* 单线程流水文件达到 1,000,000 行自动滚动切分（Rotation），防范长时间高并发测试导致的磁盘爆满与后处理 OOM。


* **一键式可视化看板 (Automated Dashboard)**
* 提供配套的 Python 脚本，支持海量分片日志的一键合并、时延排序及 P99/P99.9 长尾计算。
* 自动生成包含散点图、CDF 累积分布、TPS/带宽趋势、状态码占比的 2x2 高清诊断看板。



---

## 🛠️ 编译与安装 (Build & Install)

### 依赖项

* Linux 环境 (CentOS / Ubuntu 等)
* GCC 编译器 (支持 C99/GNU99 标准)
* 华为云 OBS C SDK (`eSDKOBS`) 及对应的 `libcurl`, `openssl` 动态库。
* Python 3.x 及 `pandas`, `matplotlib` (仅用于后期图表生成)

### 编译命令

```bash
# 清理历史产物
make clean_objs

# 编译标准版 (连接真实 OBS 环境)
make

# 编译 Mock 版 (仅用于联调逻辑，不发起真实网络请求)
make mock

```

编译成功后，将在根目录生成可执行文件 `obs_c_bench`（或 `obs_c_bench_mock`）。

---

## 🚀 快速开始 (Quick Start)

### 1. 配置凭证 (`users.dat`)

在根目录创建或编辑 `users.dat`，配置需要参与压测的账户信息（格式：`用户名, AK, SK`）：

```text
user1, YOUR_AK_1, YOUR_SK_1
user2, YOUR_AK_2, YOUR_SK_2

```

### 2. 调整测试计划 (`config.dat`)

编辑 `config.dat`，设置目标 Endpoint、并发量及测试动作。核心参数如下：

* `Users=1`：加载 `users.dat` 中的几个用户。
* `ThreadsPerUser=1000`：每个用户启动的并发线程数。
* `TestCase=201`：压测动作 (201=PUT, 202=GET, 204=DELETE, 216=MULTIPART, 230=RESUMABLE, 900=MIX)。
* `ObjectSize=4096`：测试对象的大小 (支持范围配置，如 `1024~4096`)。
* `RequestsPerThread=10000` 或 `RunSeconds=300`：退出条件限制。

*(注：详细配置说明请参考 `config.dat` 文件内的中文注释)*

### 3. 执行压测

工具默认读取当前目录下的 `config.dat`：

```bash
./obs_c_bench

```

也可通过 CLI 参数快速覆盖 TestCase，方便脚本串联执行 (例如先跑 201 PUT，再跑 202 GET)：

```bash
./obs_c_bench 201
./obs_c_bench 202

```

---

## 📊 日志与数据可视化 (Logging & Visualization)

当您在 `config.dat` 中配置了 `EnableDetailLog=true` 后，工具将为您提供企业级的分析能力。

每次运行结束后，工具会在 `logs/` 目录下生成一个**任务专属文件夹**（如 `logs/task_20260224_123045`），目录结构如下：

* `brief.txt`: 全局配置与最终汇总报告（包含总 TPS、带宽及各维度错误码统计）。
* `realtime.txt`: 每 3 秒一次的监控采样快照。
* `detail_X_partY.csv`: 高性能、多线程切割的请求级流水明细。

### 一键生成分析看板

请确保系统已安装必要的 Python 库：

```bash
pip install pandas matplotlib numpy

```

**Step 1. 合并流水并计算长尾指标**

```bash
python3 merge_details.py

```

*该脚本将自动寻找最新的 task 目录，将所有线程的分片流水按绝对时间线合并为单一的 `detail.csv`，并在控制台输出平均时延与 P99 时延。*

**Step 2. 生成可视化 Dashboard**

```bash
python3 plot_report.py

```

*该脚本读取 `detail.csv`，并在对应 task 目录下生成 `dashboard.png`。图中包含：*

1. **Latency Scatter & Trend**: 请求时延散点分布与 1秒级移动平均线（检测系统抖动毛刺）。
2. **Latency CDF**: 时延累积概率分布及 P90/P99 水位线。
3. **Instant TPS & Bandwidth**: 瞬时 TPS 与吞吐带宽的双 Y 轴趋势图（检测性能掉底现象）。
4. **Status Code Distribution**: HTTP 状态码及错误分类环形占比图。

---

## ⚠️ 注意事项 (Notes)

1. **ULIMIT 限制**：在进行高并发（如 >1000 线程）压测前，请确保操作系统已提升文件句柄上限（`ulimit -n 655350`），否则由于套接字枯竭会导致大量 SDK 内部错误。
2. **网卡与带宽**：压测极致吞吐时，请监控压测机本地的网卡带宽使用率（通过 `sar -n DEV 1` 或 `nload`），避免因客户端网卡跑满导致的时延虚高。
3. **大容量日志**：长稳压测（如 7x24 小时）开启 `EnableDetailLog=true` 会占用显著的磁盘空间（尽管已实现自动轮转切割）。请确保压测机所在磁盘空间充足。
