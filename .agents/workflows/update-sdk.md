---
description: 自动化下载/解析最新的华为云 OBS C SDK 并重新编译本工具。
---

本项目提供了一个自动化脚本 `update_sdk.py`，用于更新依赖的 SDK 并重新构建工具。

// turbo
1. 执行更新脚本：
```bash
python3 update_sdk.py
```

### 说明
- **架构自动识别**：脚本会自动检测当前系统是 x86_64 还是 aarch64，并调用相应的构建脚本（`build.sh` 或 `build_aarch.sh`）。
- **优先使用本地包**：如果项目根目录下已经存在 `master.zip`，脚本将跳过下载直接使用该压缩包。
- **自动构建**：脚本会自动解压 SDK、执行构建、替换 `lib` 目录下的动态库，并最终执行 `make all` 重新编译项目。
