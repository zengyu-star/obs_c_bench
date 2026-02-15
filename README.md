# OBS C SDK Benchmark Tool

**OBS C SDK Benchmark Tool** 是一个高性能、基于 C 语言开发的 Huawei OBS (Object Storage Service) 压测工具。它专为评估 OBS C SDK 在**高并发**、**多租户**以及**安全增强（国密/双向认证）**场景下的性能与稳定性而设计。

## 🚀 核心特性

* **多模式运行引擎**：
* **Real Mode**: 链接真实 `libeSDKOBS.so`，连接云端 OBS 服务进行压测。
* **Mock Mode**: 内置仿真 SDK (`mock_sdk.c`)，无需网络即可验证工具逻辑、线程调度及配置正确性。


* **强制多租户架构 (Multi-Tenancy)**：
* 完全摒弃全局 AK/SK，强制通过 `users.dat` 加载多用户凭证。
* 每个用户拥有独立的线程池，模拟真实的公有云多租户并发场景。
* 支持动态桶名生成 (`{ak}.{prefix}`) 或强制固定桶名模式。


* **安全增强 (Security & GM)**：
* **国密支持**: 可配置开启国密模式 (GM Mode)，支持 SM2/SM3/SM4 算法（需 SDK 支持）。
* **SSL 版本控制**: 支持精细化控制 SSL 最小/最大版本 (TLS 1.0 - 1.3)。
* **双向认证 (Mutual SSL)**: 支持配置客户端证书 (`ClientCert`) 与私钥 (`ClientKey`) 进行双向身份验证。


* **混合负载编排 (Mixed Workload)**：
* 支持脚本化定义操作序列（如 `201,202,204` 代表 `上传->下载->删除`），并指定循环次数。


* **自动化测试套件**:
* 内置 Python 脚本，一键完成 `Mock` -> `Standard` -> `Mock_ASan` -> `ASan` 全流程冒烟测试。



---

## 🛠️ 编译与构建

### 前置依赖

* GCC / Clang
* Make
* Pthread, Libcurl, OpenSSL
* **Huawei OBS C SDK** (`libeSDKOBS.so` 及其头文件，需放置在 `./lib` 和 `./include` 目录下)

### 构建命令

| 命令 | 描述 | 输出文件 |
| --- | --- | --- |
| `make` | **[标准模式]** 编译真实 SDK 版本 | `obs_c_bench` |
| `make mock` | **[仿真模式]** 编译 Mock SDK 版本 (无网络依赖) | `obs_c_bench_mock` |
| `make asan` | **[调试模式]** 真实 SDK + 内存检测 (AddressSanitizer) | `obs_c_bench_asan` |
| `make mock_asan` | **[调试模式]** Mock SDK + 内存检测 (AddressSanitizer) | `obs_c_bench_mock_asan` |
| `make clean` | 清理所有编译产物 | - |

---

## ⚙️ 配置说明

工具启动必须依赖根目录下的 `config.dat` (主配置) 和 `users.dat` (用户凭证)。

### 1. 用户凭证 (`users.dat`)

**必选**。CSV 格式，无标题行。

```csv
# Username, AccessKey, SecretKey
user_01,AK_USER_1,SK_USER_1_SECRET
user_02,AK_USER_2,SK_USER_2_SECRET
...

```

### 2. 主配置 (`config.dat`)

#### [Multi-User] 多租户并发

| 参数 | 说明 | 示例 |
| --- | --- | --- |
| `Users` | **必须**从 users.dat 加载的用户数量。若文件有效行数少于此值，工具将**报错退出**。 | `5` |
| `ThreadsPerUser` | 每个用户的并发线程数。**总并发 = Users * ThreadsPerUser**。 | `10` |
| `BucketNamePrefix` | 动态桶名后缀。桶名格式：`{ak_lower}.{prefix}`。 | `bench.test` |
| `BucketNameFixed` | (可选) 若设置，所有用户强制使用该固定桶名。 | `my-fixed-bucket` |

#### [Security] 安全与国密

| 参数 | 说明 | 示例 |
| --- | --- | --- |
| `GmModeSwitch` | 开启国密模式 (true/false)。开启后会校验 SSL 版本配置。 | `false` |
| `SslMinVersion` | SSL 最小版本 (1.0, 1.1, 1.2, 1.3)。 | `1.2` |
| `SslMaxVersion` | SSL 最大版本 (1.0, 1.1, 1.2, 1.3)。需 >= MinVersion。 | `1.3` |
| `MutualSslSwitch` | 开启双向认证 (true/false)。开启后必须配置证书路径。 | `false` |
| `ClientCertPath` | 客户端证书路径 (双向认证必填)。 | `./certs/client.crt` |
| `ClientKeyPath` | 客户端私钥路径 (双向认证必填)。 | `./certs/client.key` |
| `ClientKeyPassword` | 私钥密码 (可选)。 | `password123` |

#### [TestPlan] 测试计划

| 参数 | 说明 | 示例 |
| --- | --- | --- |
| `TestCase` | 201(Put), 202(Get), 204(Del), 216(Multipart), 230(Resumable), 900(Mix) | `201` |
| `RunSeconds` | 压测持续时间(秒)。0 表示按请求次数运行。 | `60` |
| `RequestsPerThread` | 每个线程的最大请求数 (当 RunSeconds=0 时生效)。 | `1000` |
| `MixOperation` | 混合模式操作序列 (逗号分隔)。 | `201,202,204` |
| `MixLoopCount` | 混合模式大循环次数。 | `100` |

---

## 🚀 运行测试

### 1. 手动运行

```bash
# 编译
make

# 运行 (默认读取 config.dat)
./obs_c_bench

# 运行 (CLI 强制覆盖 TestCase，例如强制跑混合模式)
./obs_c_bench 900

```

### 2. 自动化冒烟测试

使用 Python 脚本一键验证 4 种编译模式，确保代码在不同环境下的健壮性。

```bash
python3 compile_and_smoke_test.py

```

* 脚本会自动生成临时测试文件 (`test_data.bin`)。
* 自动捕获 `Failed` 计数，任何失败都会导致测试中止。
* 执行顺序：`Mock` -> `Standard` -> `Mock_ASan` -> `ASan` (Fail-Fast 策略)。

---

## 📂 目录结构

```text
.
├── src/                # 源代码
│   ├── main.c          # 入口、多租户调度、结果统计
│   ├── worker.c        # 核心压测循环
│   ├── obs_adapter.c   # SDK 适配层 (含国密/SSL配置注入)
│   ├── mock_sdk.c      # 仿真 SDK 实现 (用于 mock 模式)
│   ├── config_loader.c # 配置文件解析与强校验逻辑
│   ├── log.c           # 日志模块
│   └── bench.h         # 核心头文件
├── include/            # SDK 头文件 (eSDKOBS.h, mock_eSDKOBS.h)
├── lib/                # 依赖库 (libeSDKOBS.so)
├── logs/               # 运行时日志
├── config.dat          # 主配置文件
├── users.dat           # 用户凭证文件
├── Makefile            # 构建脚本
├── compile_and_smoke_test.py # 自动化测试脚本
└── README.md           # 说明文档

```

## ⚠️ 注意事项

1. **全局 AK/SK 已移除**: 不要在 `config.dat` 中寻找 AK/SK 配置，必须使用 `users.dat`。
2. **国密模式依赖**: 开启 `GmModeSwitch=true` 前，请确认链接的 `libeSDKOBS.so` 是支持国密算法的版本，否则可能会在运行时报错或回退。
3. **性能调优**: 在高并发场景 (`Total Threads > 1000`) 下，请确保操作系统 `ulimit -n` (文件句柄) 和 `ulimit -u` (进程数) 已适当调大。