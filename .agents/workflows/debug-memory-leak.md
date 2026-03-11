---
description: 自动化执行 Valgrind 内存检查并总结 C 语言层面的内存泄漏报告。
---

// turbo-all
1. 清除旧的编译输出并重新编译带有 Debug symbol 的版本（执行 `make clean && make all`）。
2. 在项目根目录下执行内存测试：`valgrind --leak-check=full --show-leak-kinds=all ./obs_c_bench`，标准输出和错误重定向到 `mem_report.log`。
3. 读取生成的 `mem_report.log`，提取出所有的 "definitely lost" 和 "indirectly lost" 的堆栈追踪。
4. 结合 `mcp_server.py` 的调用逻辑，为我总结哪些模块（如网络层、解析层）可能漏掉了 `free()`。