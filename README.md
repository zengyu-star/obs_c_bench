# Huawei Cloud OBS C SDK Benchmark Tool

`obs_c_bench` 是一款专为华为云对象存储服务（OBS）设计的高性能、企业级 C 语言压测工具。它底层直接对接 OBS C SDK，旨在评估与调优极端并发场景下的存储集群性能、SDK 稳定性以及网络边界极限。

## ✨ 核心特性

* **极致的高并发发流模型**：基于 `pthreads` 的原生多线程模型，支持极大规模并发（如 100 线程/每用户），配合严格的时序隔离，提供精准的 TPS 与吞吐量（MB/s）基准统计。
* **企业级 STS 临时安全凭证支持**：原生集成 `generate_temp_ak_sk.py` 脚本，支持基于 IAM 密码的动态 Token 换取，完全适配云原生严格权限管控场景。
* **零拷贝数据一致性校验 (Zero-Copy Validation)**：在 GET 压测中，利用动态生成的 1MB 特征环形缓冲区直接在底层网络回调中执行 `memcmp`，在不增加内存拷贝开销的前提下精准捕获**静默数据损坏 (Silent Data Corruption)** 和数据截断。
* **打散服务端热点 (LCG Hash Prefix)**：支持对生成的对象名自动附加 LCG 伪随机哈希前缀，物理打散后端存储分片，避免压测时的元数据热点。
* **防假死与暴力逃生机制 (Escalated Shutdown)**：深度定制的 `SIGINT` 信号处理器。单次 `Ctrl+C` 触发优雅退出；当底层 glibc 或网络栈陷入死锁时，二次 `Ctrl+C` 即可实施内核级 `_exit(1)` 强制逃生。
* **Mock 离线调试模式**：内置桩代码（Mock SDK），允许在无网络或无真实华为云环境的单机上进行百万级 TPS 的代码逻辑演练。

---

## 🛠️ 编译与安装

本工具要求 Linux 环境，并已安装 GCC 和 Make。

### 1. 编译真实业务版
默认链接真实的 `libeSDKOBS.so`，用于真实的打流压测：
```bash
make clean
make all
生成可执行文件: obs_c_bench

2. 编译 Mock 离线仿真版
剥离真实网络 IO，直接在内存中模拟 SDK 回调，用于排查应用层死锁、统计逻辑验证以及极高并发下的线程调度测试：

Bash
make clean
make mock
生成可执行文件: obs_c_bench_mock

(注：本工程还提供 make asan 和 make mock_asan 用于内存泄漏检测，需依赖 libasan 库)

⚙️ 核心配置指南 (config.dat)
压测的全部行为由当前目录下的 config.dat 驱动。以下是几个最重要的配置项说明：

并发与用例设置
Users: 模拟的租户数量（从用户文件中读取的行数）。

ThreadsPerUser: 为每个用户分配的独立工作线程数。总并发 = Users × ThreadsPerUser。

TestCase: 执行的压测指令。201 (PUT), 202 (GET), 204 (DELETE), 216 (分段上传), 900 (生命周期混合操作)。

RunSeconds: 压测时长（秒）。设置为 0 表示不限时，按请求数退出。

临时凭证与桶路由策略 (重要 ⚠️)
IsTemporaryToken: 设置为 true 时，工具会阻塞等待 Python 脚本从 IAM 换取临时 Token，然后才开始精准计时发流。

BucketNameFixed: 目标桶名。当启用 STS 临时凭证时，必须强制指定此固定桶名，以防止临时生成的随机 AK 导致桶路由解析失败引发 403 错误。

ObjNamePatternHash: 强烈建议在基准测试中设为 true，以避免顺序命名导致服务端热点。

可靠性与校验
ConnectTimeoutSec / RequestTimeoutSec: libcurl 底层的毫秒级连接与传输超时时间控制。

EnableDataValidation: GET 校验开关，捕获底层传输错误。开启后失败计数将体现在 Internal Validation Fail 中。

🔐 凭证文件与 STS Token 换取
本工具支持两种凭证输入模式：

模式一：长期 AK/SK 模式 (IsTemporaryToken=false)
工具读取 users.dat，格式为：

Plaintext
Username,AccessKey,SecretKey
user1,ABCDEFG12345,aBcDeFg67890
模式二：动态 STS 凭证模式 (IsTemporaryToken=true)
此模式下，users.dat 的第一列必须是用于 IAM 认证的 账户ID-用户名-密码 组合：

Plaintext
# 格式: AccountID-IAMUsername-IAMPassword,原有AK,原有SK
7ffaf6...-niczeger-Password123!,ak_ignored,sk_ignored
启动压测时，C 工具将自动挂起，调用内置的 generate_temp_ak_sk.py 脚本生成 temptoken.dat，最后将其中的超长 SecurityToken 动态注入到 SDK 中。

🚀 运行与输出观测
启动命令
Bash
./obs_c_bench          # 使用默认的 config.dat
# 或
./obs_c_bench test.dat # 指定配置文件
运行时控制台输出
Plaintext
[Monitor] RunTime:     3.0s | Process:   5.00% | Cumul TPS:  1500.20 | Cumul BW:  15.20 MB/s | Success Rate: 100.00% | Total Reqs: 4500
压测产物 (logs/task_YYYYMMDD_HHMMSS/)
brief.txt: 压测结束后的最终汇总报告，包含配置快照、各 HTTP 状态码（403/404/5xx）的精准报错分类，以及最终的 TPS 与吞吐量。

realtime.txt: 逗号分隔的 CSV 格式时序数据流（包含时间戳、进度、TPS、带宽），可直接用于 Python/Excel 绘制性能衰减与波动图表。

detail.csv: (当 EnableDetailLog=true 时生成) 记录每一次 HTTP 请求的绝对时间戳、对象 Key、耗时（ms）与响应码，用于长尾时延（P99/P999）分析。
