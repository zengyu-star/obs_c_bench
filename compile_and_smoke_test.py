#!/usr/bin/env python3
# -*- coding: utf-8 -*-

import subprocess
import os
import shutil
import time
import sys
import re

# ================= 配置区域 =================
CONFIG_FILE = 'config.dat'
CONFIG_BAK = 'config.dat.bak'
LIB_DIR = './lib'
CACHE_DIR = './test_bin_cache'  # 存放编译产物的临时目录
TEST_DATA_FILE = './test_data.bin' # 本地测试数据文件
TEST_DATA_SIZE_MB = 5           # 测试数据大小 (MB)

# 测试用例 ID
TEST_CASES = [201, 202, 204, 216, 230, 900]
TEST_DURATION = 3 

# 编译任务顺序: (显示名称, Make命令, 产物文件名)
# 顺序: Mock -> Standard -> Mock_ASan -> ASan
BUILD_TASKS = [
    ("Mock",        "make mock",        "obs_c_bench_mock"),
    ("Standard",    "make",             "obs_c_bench"),
    ("Mock_ASan",   "make mock_asan",   "obs_c_bench_mock_asan"),
    ("ASan",        "make asan",        "obs_c_bench_asan")
]
# ===========================================

class BenchmarkTester:
    def __init__(self):
        self.results = []
        self.work_dir = os.path.dirname(os.path.abspath(__file__))
        os.chdir(self.work_dir)
        
        # 设置环境变量
        self.env = os.environ.copy()
        lib_path = os.path.abspath(LIB_DIR)
        if 'LD_LIBRARY_PATH' in self.env:
            self.env['LD_LIBRARY_PATH'] = f"{lib_path}:{self.env['LD_LIBRARY_PATH']}"
        else:
            self.env['LD_LIBRARY_PATH'] = lib_path

    def run_cmd(self, cmd):
        """执行命令并返回 (returncode, stdout+stderr)"""
        try:
            result = subprocess.run(
                cmd, shell=True, capture_output=True, text=True, env=self.env
            )
            return result.returncode, result.stdout + result.stderr
        except Exception as e:
            return -1, str(e)

    def prepare_env(self):
        print("[Init] Preparing environment...")
        
        # 1. 创建缓存目录
        if os.path.exists(CACHE_DIR):
            shutil.rmtree(CACHE_DIR)
        os.makedirs(CACHE_DIR)

        # 2. [新增] 检查并生成测试数据文件
        if not os.path.exists(TEST_DATA_FILE):
            print(f"[Init] Generating {TEST_DATA_SIZE_MB}MB test data: {TEST_DATA_FILE} ...")
            # 使用 /dev/urandom 生成随机数据，避免压缩算法作弊
            cmd = f"dd if=/dev/urandom of={TEST_DATA_FILE} bs=1M count={TEST_DATA_SIZE_MB} status=none"
            ret, _ = self.run_cmd(cmd)
            if ret != 0:
                print(f"[Error] Failed to generate {TEST_DATA_FILE}")
                sys.exit(1)
        else:
            print(f"[Init] Test data exists: {TEST_DATA_FILE}")

        # 3. 备份并修改 Config
        if os.path.exists(CONFIG_FILE):
            shutil.copy(CONFIG_FILE, CONFIG_BAK)
        else:
            # 创建默认 Config
            with open(CONFIG_FILE, 'w') as f:
                f.write(f"Endpoint=obs.example.com\nAK=test\nSK=test\nBucket=test\nUsers=1\nThreadsPerUser=1\nTestCase=201\nRunSeconds=3\nUploadFilePath={TEST_DATA_FILE}\n")
        
        # 动态修改 Config: 运行时间和 UploadFilePath
        sed_cmds = [
            f"sed -i 's/^RunSeconds=.*/RunSeconds={TEST_DURATION}/g' {CONFIG_FILE}",
            # 确保 UploadFilePath 指向我们生成的文件
            f"sed -i 's|^UploadFilePath=.*|UploadFilePath={TEST_DATA_FILE}|g' {CONFIG_FILE}"
        ]
        for cmd in sed_cmds:
            self.run_cmd(cmd)

    def restore_env(self):
        # 恢复 Config
        if os.path.exists(CONFIG_BAK):
            shutil.move(CONFIG_BAK, CONFIG_FILE)
        # 清理缓存
        if os.path.exists(CACHE_DIR): shutil.rmtree(CACHE_DIR)

    def parse_stats(self, output):
        """解析 C 工具输出"""
        stats = {"success": 0, "failed": 0}
        m_failed = re.search(r"Failed:\s+(\d+)", output)
        if m_failed: stats["failed"] = int(m_failed.group(1))
        m_success = re.search(r"Success:\s+(\d+)", output)
        if m_success: stats["success"] = int(m_success.group(1))
        return stats

    def stage_compile_all(self):
        """Stage 1: 编译所有版本并缓存"""
        print("\n" + "=" * 60)
        print(">>> Stage 1: Compilation Check (Fail-Fast)")
        print("=" * 60)
        
        for name, make_cmd, bin_name in BUILD_TASKS:
            print(f"[{name}] Compiling...", end='', flush=True)
            
            self.run_cmd("make clean")
            start_t = time.time()
            ret, output = self.run_cmd(make_cmd)
            duration = time.time() - start_t
            
            if ret != 0:
                print(f" FAIL! ({duration:.1f}s)")
                print(f"Error Log:\n{output[-1000:]}") 
                return False
            
            if not os.path.exists(bin_name):
                print(f" FAIL! (Binary {bin_name} not generated)")
                return False
            
            dst_path = os.path.join(CACHE_DIR, bin_name)
            shutil.move(bin_name, dst_path)
            os.chmod(dst_path, 0o755)
            
            print(f" PASS ({duration:.1f}s) -> Cached")
            
        print("All compilations successful.\n")
        return True

    def stage_smoke_test(self):
        """Stage 2: 使用缓存的二进制进行冒烟测试"""
        print("\n" + "=" * 60)
        print(">>> Stage 2: Smoke Testing (Mock -> Std -> Mock_ASan -> ASan)")
        print("=" * 60)
        
        for name, _, bin_name in BUILD_TASKS:
            bin_path = os.path.join(CACHE_DIR, bin_name)
            print(f"\n--- Testing Build: {name} ---")
            
            for case in TEST_CASES:
                print(f"  Case {case:<3} ... ", end='', flush=True)
                
                start_t = time.time()
                ret, output = self.run_cmd(f"{bin_path} {case}")
                duration = time.time() - start_t
                
                stats = self.parse_stats(output)
                status = "PASS"
                detail = ""

                # --- 判定逻辑 ---
                if ret != 0:
                    status = "FAIL"
                    detail = f"Crash(Exit {ret})"
                    if "AddressSanitizer" in output: detail = "ASan Error"
                elif "AddressSanitizer" in output:
                    status = "FAIL"
                    detail = "ASan Error (Exit 0)"
                elif stats['failed'] > 0:
                    status = "FAIL"
                    detail = f"Business Fail ({stats['failed']} errs)"
                elif stats['success'] == 0:
                    status = "WARN"
                    detail = "0 Success"
                # ----------------

                print(f"{status} (Succ:{stats['success']}, Fail:{stats['failed']}, {duration:.1f}s)")
                
                self.results.append({
                    "Build": name,
                    "Case": case,
                    "Status": status,
                    "Detail": detail
                })

    def print_summary(self):
        print("\n" + "=" * 60)
        print(f"{'BUILD':<12} | {'CASE':<6} | {'STATUS':<10} | {'DETAIL'}")
        print("-" * 60)
        
        pass_count = 0
        total_count = 0
        failed_tests = []

        for r in self.results:
            total_count += 1
            if r['Status'] == 'PASS':
                pass_count += 1
            else:
                failed_tests.append(r)
            
            print(f"{r['Build']:<12} | {r['Case']:<6} | {r['Status']:<10} | {r['Detail']}")
            
        print("-" * 60)
        print(f"Summary: {pass_count}/{total_count} Passed")
        
        if failed_tests:
            print("\nFAILED TESTS:")
            for r in failed_tests:
                print(f" - {r['Build']} Case {r['Case']}: {r['Detail']}")
            sys.exit(1)
        else:
            print("\nALL PASSED.")
            sys.exit(0)

    def run(self):
        try:
            self.prepare_env()
            if not self.stage_compile_all():
                print(">>> ABORTING: Compilation failed.")
                sys.exit(1)
            self.stage_smoke_test()
            self.print_summary()
        except KeyboardInterrupt:
            print("\nInterrupted.")
        finally:
            self.restore_env()

if __name__ == "__main__":
    BenchmarkTester().run()