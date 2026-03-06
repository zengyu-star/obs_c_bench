#!/usr/bin/env python3
# -*- coding: utf-8 -*-

"""
OBS C-Bench Functional E2E Test Script (Pytest version)
=======================================================
Run this file directly: python3 functional_test.py
It will automatically invoke pytest and save results to report.html.
"""

import os
import sys
import shutil
import subprocess
import re

# ==========================================
# 1. Dependency Check (Try-Catch)
# ==========================================
try:
    import pytest
except ImportError:
    print("\n[Error] Missing required dependency: pytest")
    print("Please install it using: pip install pytest")
    sys.exit(1)

HAS_PYTEST_HTML = True
try:
    import pkg_resources
    pkg_resources.require("pytest-html")
except Exception:
    HAS_PYTEST_HTML = False

# ==========================================
# 2. Main Execution Wrapper
# ==========================================
if __name__ == "__main__":
    current_file = os.path.abspath(__file__)
    print(f"Starting automated E2E testing for obs_c_bench...")
    
    pytest_args = [current_file, "-v"]
    if HAS_PYTEST_HTML:
        print("HTML reporting enabled. Saving output to: report.html")
        pytest_args.extend(["--html=report.html", "--self-contained-html"])
    else:
        print("\n[Warning] Missing optional dependency: pytest-html")
        print("Install it using 'pip install pytest-html' to generate beautiful HTML reports.")
        print("Running in console-only mode...\n")
        
    # Programmatically invoke pytest on this very file
    sys.exit(pytest.main(pytest_args))


# ==========================================
# 3. Global Test Configuration & Utilities
# ==========================================
WORK_DIR = os.path.dirname(os.path.abspath(__file__))
if not WORK_DIR:
    WORK_DIR = os.getcwd()

CONFIG_FILE = os.path.join(WORK_DIR, 'config.dat')
CONFIG_BAK = os.path.join(WORK_DIR, 'config.dat.bak')
USERS_FILE = os.path.join(WORK_DIR, 'users.dat')
USERS_BAK = os.path.join(WORK_DIR, 'users.dat.bak')
LIB_DIR = os.path.join(WORK_DIR, 'lib')
BINARY = os.path.join(WORK_DIR, 'obs_c_bench')

def run_cmd(cmd):
    env = os.environ.copy()
    if 'LD_LIBRARY_PATH' in env:
        env['LD_LIBRARY_PATH'] = f"{LIB_DIR}:{env['LD_LIBRARY_PATH']}"
    else:
        env['LD_LIBRARY_PATH'] = LIB_DIR
        
    result = subprocess.run(cmd, shell=True, capture_output=True, text=True, env=env, cwd=WORK_DIR)
    return result.returncode, result.stdout + result.stderr

def update_config(key, value):
    if not os.path.exists(CONFIG_FILE):
         pytest.fail(f"Configuration file missing: {CONFIG_FILE}")
    with open(CONFIG_FILE, 'r') as f:
        lines = f.readlines()
    with open(CONFIG_FILE, 'w') as f:
        found = False
        for line in lines:
            if line.strip().startswith(f"{key}="):
                f.write(f"{key}={value}\n")
                found = True
            else:
                f.write(line)
        if not found:
            f.write(f"{key}={value}\n")

def check_obs_output(output, expect_success=True, expected_string=None):
    success_num = 0
    fail_num = 0
    m_failed = re.search(r"Failed:\s+(\d+)", output)
    if m_failed: fail_num = int(m_failed.group(1))
    m_success = re.search(r"Success:\s+(\d+)", output)
    if m_success: success_num = int(m_success.group(1))
    
    assert "Segmentation fault" not in output, "CRITICAL: Segmentation Fault detected!"
    assert "core dumped" not in output, "CRITICAL: Core dumped detected!"

    if expect_success:
        assert fail_num == 0, f"Expected 0 failures, got {fail_num}. Output:\n{output}"
        assert success_num > 0, f"Expected >0 successes, got {success_num}. Output:\n{output}"
        if expected_string:
            assert expected_string in output, f"Missing string '{expected_string}' in output."
    else:
        assert fail_num > 0 or "Error" in output or "Failed" in output, f"Expected process to fail or catch error, but it succeeded. Output:\n{output}"
        if expected_string:
            assert expected_string in output, f"Missing string '{expected_string}' in output."


# ==========================================
# 4. Pytest Fixtures (Setup & Teardown)
# ==========================================

@pytest.fixture(scope="session", autouse=True)
def prepare_environment():
    print("\n[Setup] Running make clean && make all...")
    ret, out = run_cmd("make clean && make all")
    if ret != 0 or not os.path.exists(BINARY):
        pytest.fail(f"Failed to compile the tool!\n{out}")
        
    print("[Setup] Generating boundary size test files (0B, 1B, 5MB)...")
    run_cmd(f"touch {os.path.join(WORK_DIR, '0byte.bin')}")
    run_cmd(f"dd if=/dev/urandom of={os.path.join(WORK_DIR, '1byte.bin')} bs=1 count=1 status=none")
    run_cmd(f"dd if=/dev/urandom of={os.path.join(WORK_DIR, '5mb.bin')} bs=1M count=5 status=none")
    
    yield
    
    print("\n[Teardown] Cleaning up boundary files...")
    for f in ['0byte.bin', '1byte.bin', '5mb.bin']:
        f_path = os.path.join(WORK_DIR, f)
        if os.path.exists(f_path):
            os.remove(f_path)


@pytest.fixture(scope="function", autouse=True)
def isolate_config():
    if os.path.exists(CONFIG_FILE): shutil.copy(CONFIG_FILE, CONFIG_BAK)
    if os.path.exists(USERS_FILE): shutil.copy(USERS_FILE, USERS_BAK)
    
    update_config("ThreadsPerUser", "2")
    update_config("RunSeconds", "0")
    update_config("RequestsPerThread", "5")
    update_config("EnableDataValidation", "false")
    
    yield
    
    if os.path.exists(CONFIG_BAK): shutil.move(CONFIG_BAK, CONFIG_FILE)
    if os.path.exists(USERS_BAK): shutil.move(USERS_BAK, USERS_FILE)


# ==========================================
# 5. Test Cases
# ==========================================

class TestDataConsistency:
    def test_put_201_validation_on(self):
        update_config("EnableDataValidation", "true")
        update_config("ObjectSize", "1048576")
        ret, out = run_cmd(f"{BINARY} 201")
        assert ret == 0, f"Binary exited with code {ret}"
        check_obs_output(out, expect_success=True)

    def test_get_202_validation_on(self):
        update_config("EnableDataValidation", "true")
        update_config("ObjectSize", "1048576")
        ret, out = run_cmd(f"{BINARY} 202")
        assert ret == 0
        check_obs_output(out, expect_success=True)

    def test_multipart_put_216_validation_on(self):
        update_config("EnableDataValidation", "true")
        update_config("ObjectSize", "6291456")
        ret, out = run_cmd(f"{BINARY} 216")
        assert ret == 0
        check_obs_output(out, expect_success=True)
        
    def test_resumable_get_230_validation_on(self):
        update_config("EnableDataValidation", "true")
        update_config("ObjectSize", "6291456")
        ret, out = run_cmd(f"{BINARY} 230")
        assert ret == 0
        check_obs_output(out, expect_success=True)

    def test_get_202_validation_fail_format(self):
        update_config("EnableDataValidation", "false")
        update_config("ObjectSize", "1048576")
        ret, out = run_cmd(f"{BINARY} 201")
        assert ret == 0
        check_obs_output(out, expect_success=True)

        update_config("EnableDataValidation", "true")
        ret, out = run_cmd(f"{BINARY} 202")
        check_obs_output(out, expect_success=False)


class TestMixedOperations:
    def test_case_900_mixed_ops(self):
        update_config("ObjectSize", "1024")
        update_config("RequestsPerThread", "20") 
        ret, out = run_cmd(f"{BINARY} 900")
        assert ret == 0
        check_obs_output(out, expect_success=True)


class TestBoundarySizes:
    def test_0_byte_upload(self):
        update_config("UploadFilePath", os.path.join(WORK_DIR, "0byte.bin"))
        ret, out = run_cmd(f"{BINARY} 201")
        assert ret == 0
        check_obs_output(out, expect_success=True)

    def test_0_byte_download(self):
        update_config("UploadFilePath", os.path.join(WORK_DIR, "0byte.bin"))
        ret, out = run_cmd(f"{BINARY} 202")
        assert ret == 0
        check_obs_output(out, expect_success=True)

    def test_1_byte_upload(self):
        update_config("UploadFilePath", os.path.join(WORK_DIR, "1byte.bin"))
        ret, out = run_cmd(f"{BINARY} 201")
        assert ret == 0
        check_obs_output(out, expect_success=True)

    def test_5_mb_upload(self):
        update_config("UploadFilePath", os.path.join(WORK_DIR, "5mb.bin"))
        ret, out = run_cmd(f"{BINARY} 201")
        assert ret == 0
        check_obs_output(out, expect_success=True)


class TestErrorHandling:
    def test_404_not_found(self):
        run_cmd(f"{BINARY} 204")
        ret, out = run_cmd(f"{BINARY} 202")
        check_obs_output(out, expect_success=False)
        assert "404" in out or "NotFound" in out

    def test_403_auth_failure(self):
        if os.path.exists(USERS_FILE):
             with open(USERS_FILE, 'w') as f:
                 f.write("user1, INVALID_AK, INVALID_SK\n")
             ret, out = run_cmd(f"{BINARY} 201")
             check_obs_output(out, expect_success=False)
             assert "403" in out or "Forbidden" in out
        else:
             pytest.skip("users.dat not found, skipping auth test.")

    def test_invalid_dns_endpoint(self):
        update_config("Endpoint", "invalid.obs.example.com")
        ret, out = run_cmd(f"{BINARY} 201")
        check_obs_output(out, expect_success=False)


class TestAdvancedFeatures:
    def test_fixed_bucket_name(self):
        valid_bucket = "bench.test"
        try:
            with open(USERS_FILE, 'r') as f:
                for line in f:
                    if ',' in line:
                        ak = line.split(',')[1].strip().lower()
                        valid_bucket = f"{ak}.bench.test"
                        break
        except Exception:
            pass

        update_config("BucketNameFixed", valid_bucket)
        ret, out = run_cmd(f"{BINARY} 201")
        assert ret == 0
        check_obs_output(out, expect_success=True)

    def test_temporary_credentials(self):
        tmp_cred_path = os.path.join(WORK_DIR, "users.dat_tmp_cred")
        if os.path.exists(tmp_cred_path):
             shutil.copy(tmp_cred_path, USERS_FILE)
             update_config("IsTemporaryToken", "true")
             ret, out = run_cmd(f"{BINARY} 201")
             assert ret == 0
             check_obs_output(out, expect_success=True)
        else:
             pytest.skip(f"Temp cred file '{tmp_cred_path}' not found, skipping test.")
