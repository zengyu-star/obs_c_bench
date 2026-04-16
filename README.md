# OBS C SDK Benchmark Tool (obs_c_bench)

![C Standard](https://img.shields.io/badge/C-C99%2B-blue.svg)
![License](https://img.shields.io/badge/License-Apache_2.0-green.svg)
![Build](https://img.shields.io/badge/Build-Make-orange.svg)

`obs_c_bench` 是一款专为华为云对象存储服务 (OBS) 打造的工业级、高性能 C 语言压测工具。它基于官方 OBS C SDK 开发，采用了极致的无锁并发架构，旨在帮助开发者、架构师和测试工程师准确评估云存储后端的极限 TPS、吞吐带宽以及长尾时延表现。

---

## 🌟 核心特性 (Key Features)

* **极致的并发性能 (Lock-Free Architecture)**: Worker 线程执行请求及本地统计数据收集时**全程无锁**，榨干压测机每一滴 CPU 性能。独立的旁路监控线程（Monitor Thread）每 3 秒无锁采集全局状态，实时输出累计 TPS、带宽与成功率，对发流性能 **0 干扰**。
* **确定性极速哈希打散 (High-Performance 128-bit Hash)**: 内置高性能 SplitMix64 位混合算法。开启 `ObjNamePatternHash=true` 后，会自动计算出一个 32 位十六进制字符的随机前缀，**彻底消除云存储底层分片的热点瓶颈**。算法具备极高性能（纳秒级 CPU 混合）与强确定性，确保先执行 PUT 压测后，再次执行 GET 压测能够 100% 精确命中已上传的对象。
* **零拷贝异构数据校验 (Zero-Copy Validation)**: 支持 `EnableDataValidation=true`。工具在内存中预分配 1MB 的确定性特征环形缓冲区（Pattern Buffer）。下载过程在网络回调层实时计算绝对偏移量，进行异构比对。即使并发 Range 下载，也能在极低 CPU 消耗下完成严格的**数据一致性校验**，精准捕获静默错误 (DataConsistencyError)。
* **智能多租户桶路由 (Smart Bucket Routing)**: 支持 `users.dat` 批量加载多账户。动态桶名拼接策略：自动按照 `{ak_lowercase}.{BucketNamePrefix}` 的格式将流量路由至各账户的专属桶，或通过 `BucketNameFixed` 强制打向固定桶。
* **防爆内存的海量流水落盘 (Log Rotation)**: 开启 `EnableDetailLog=true` 后，支持请求级明细流水落盘。工具自动按任务时间戳创建独立隔离目录。单线程流水文件达到大记录数自动滚动切分（Rotation），防范长时间高并发测试导致的磁盘爆满与后处理 OOM。

---

## 🛠️ 编译与安装 (Build & Install)

### 1. 依赖项
请确保您的环境安装以下依赖：
* Linux 环境 (CentOS / Ubuntu 等)
* GCC 编译器 (支持 C99/GNU99 标准)
* 华为云 OBS C SDK 的 lib 库 ([下载参考](https://support.huaweicloud.com/sdk-c-devg-obs/obs_20_0004.html))
* Python 3.x 及 `pandas`, `matplotlib` (仅用于后期图表生成)

### 2. 编译命令
```bash
# 清理历史产物
make clean_objs

# 编译标准版 (连接真实 OBS 环境)
make

# 编译 Mock 版 (仅用于联调逻辑，不发起真实网络请求)
make mock

# 编译 ASAN 版本 (用于检测内存错误)
make asan

# 编译 Mock + ASAN 版本 (用于联调逻辑)
make mock_asan
```
编译成功后，将在根目录生成可执行文件 `obs_c_bench` 或 `obs_c_bench_mock`。

### 3. 自动化 SDK 更新 (可选)
使用配套脚本自动下载、编译最新的 OBS C SDK 并重新编译本工具：
```bash
python3 update_sdk.py
```

---

## ⚙️ 参数配置指南 (Configuration)

### `config.dat` (压测场景参数)

```properties
# ==================== 网络与认证 =====================
Endpoint=obs.cn-north-4.myhuaweicloud.com   # 目标区域地址
Protocol=https                              # 协议选择 (http/https)
IsTemporaryToken=false                      # 是否启用 STS 临时凭证

# ==================== 存储与桶策略 =====================
BucketNameFixed=zengyu-test-0405            # 固定桶名 (最高优先级)
BucketNamePrefix=bench-bucket               # 动态前缀 (若 Fixed 为空，格式为 {ak_lowercase}.prefix)
BucketLocation=cn-north-4                   # 桶所在区域 (Case 101/104 必填)

# ==================== 对象属性 =====================
TestCase=201                                # 压测动作指令 (详见下方列表)
KeyPrefix=c-bench-test                      # 对象名前缀
ObjectSize=4096                             # 单个对象大小 (字节)，支持范围配置如 1024~4096
PartSize=5242880                            # 分段上传单段大小 (字节)
EnableDataValidation=false                  # 是否开启强一致性数据校验
EnableDetailLog=true                        # 是否开启详细请求日志记录 (detail.csv)
UploadFilePath=test_data.bin                # 断点续传本地文件路径 (Case 230 必填)

# ==================== 并发控制 =====================
Users=1                                     # 并发用户数 (加载 users.dat 中的前几个账户)
ThreadsPerUser=1000                         # 单一用户压测线程数
RequestsPerThread=10000                     # 单线程发流请求数上限 (达到即退出)
RunSeconds=300                              # 全局运行时间上限 (秒)
```

**支持的 `TestCase` 列表**：
- `101`: **创建桶** (`CreateBucket`)
- `104`: **删除桶** (`DeleteBucket`)
- `201`: **上传对象** (`PutObject`)
- `202`: **下载对象** (`GetObject`)
- `204`: **删除对象** (`DeleteObject`)
- `216`: **分段上传** (`MultipartUpload`)
- `230`: **断点续传** (`ResumableUploadFile`)
- `900`: **混合模式** (`MixMode`)

> [!NOTE]
> * 对于 **混合模式 (900)**，需配合 `MixOperation`（如 `201,202,204`） 和 `MixLoopCount` 使用。
> * 支持以 CLI 传参方式覆盖 `TestCase`，如执行 `./obs_c_bench 202` 以快速启动下载压测。

### `users.dat` (多租户凭证)

⚠️ **安全警告**：由于此处包含真实 AK/SK，请勿将此文件提交到代码仓库。
```csv
# username, ak, sk
user1, YOUR_AK_1, YOUR_SK_1
user2, YOUR_AK_2, YOUR_SK_2
```
若使用 STS 临时凭证模式 (`IsTemporaryToken=true`)，格式调整为 `UserID-AccountName-Password,OriginalAK,OriginalSK`，启动时将自动调用 `python3 generate_temp_ak_sk.py`。

---

## 🚀 执行与自动化 (Execution & Automation)

### 执行压测
工具默认读取当前目录下的 `config.dat`：
```bash
./obs_c_bench
```

可通过 CLI 参数覆盖 TestCase 实现串联执行：
```bash
./obs_c_bench 201
./obs_c_bench 202
```

### 自动化回归测试
为了确保修改未破坏主体逻辑，项目内置了全自动化冒烟测试脚本 `compile_and_smoke_test.py`：
```bash
python3 compile_and_smoke_test.py
```
> [!TIP]
> 脚本会自动覆盖 Mock、Standard、STS 场景下的完整生命周期 (创桶 -> 核心操作 -> 删桶)，并生成对应的 PASS/FAIL 测试报告。

---

## 📈 日志与数据看板 (Reporting & Dashboard)

开启 `EnableDetailLog=true` 后，每次运行会在 `logs/` 目录下生成任务文件夹。目录结构如下：
* `brief.txt`: 全局配置与汇总报告（总 TPS、带宽及长尾时延快照）。
* `realtime.txt`: 每 3 秒一采样的运行监控状态。
* `detail_X_partY.csv`: 高性能、多线程切割的底层请求明细流水。

**一键生成数据看板**：
```bash
# 合并流水文件
python3 merge_details.py

# 利用 detail.csv 生成可视化 Dashboard 图表 (dashboard.png)
python3 plot_report.py
```
*自动生成的图表包含：时延散点分布与移动平均、CDF 长尾分布水位、瞬时 TPS 与带宽趋势、HTTP 状态码占比。*

---

## ⚠️ 注意事项 (Notes)

1. **ULIMIT 限制**：在进行高并发（如 >1000 线程）压测前，请确保操作系统已提升文件句柄上限（`ulimit -n 655350`），否则由于套接字枯竭会导致大量错误。
2. **网卡与带宽**：压测极致吞吐时，请监控压测机本地的网卡带宽使用率（通过 `sar -n DEV 1`），避免因客户端网卡跑满导致的时延虚高。
3. **大容量日志**：长稳压测开启流水落盘会占用显著磁盘空间，请提前预留足够存储容量。
