import sys
import requests
import json
from datetime import datetime

# Huawei Cloud IAM Configuration
IAM_BASE_URL = "https://iam.myhuaweicloud.com"
TOKEN_PATH = "/v3/auth/tokens"
STS_PATH = "/v3.0/OS-CREDENTIAL/securitytokens"

# File Configuration
INPUT_FILE = 'users.dat'
OUTPUT_FILE = 'temptoken.dat'

def get_temporary_credentials(user_id, user_name, password):
    """
    Fetches STS credentials. 
    Returns ((AK, SK, Token), None) if successful, else (None, ErrorMessage).
    """
    headers = {'Content-Type': 'application/json'}
    
    auth_payload = {
        "auth": {
            "identity": {
                "methods": ["password"],
                "password": {
                    "user": {"id": user_id, "password": password}
                }
            },
            "scope": {"domain": {"name": user_name}}
        }
    }

    try:
        # Step 1: Authentication
        resp = requests.post(f"{IAM_BASE_URL}{TOKEN_PATH}", json=auth_payload, headers=headers, timeout=10)
        if resp.status_code != 201:
            return None, f"AUTH_ERR_{resp.status_code}"
        
        perm_token = resp.headers.get("X-Subject-Token")

        # Step 2: STS Token Exchange
        # Duration can be adjusted (default here is 3600 seconds = 1 hour)
        sts_headers = {'Content-Type': 'application/json', 'X-Auth-Token': perm_token}
        sts_payload = {
            "auth": {
                "identity": {
                    "methods": ["token"],
                    "token": {"id": perm_token, "duration-seconds": 3600}
                }
            }
        }
        
        resp_sts = requests.post(f"{IAM_BASE_URL}{STS_PATH}", json=sts_payload, headers=sts_headers, timeout=10)
        if resp_sts.status_code != 201:
            return None, f"STS_ERR_{resp_sts.status_code}"
            
        sts_data = resp_sts.json().get('credential', {})
        
        return (sts_data.get('access'), sts_data.get('secret'), sts_data.get('securitytoken')), None

    except requests.exceptions.RequestException as e:
        return None, f"NET_ERR: {str(e)}"

def main():
    print(f"[{datetime.now().strftime('%H:%M:%S')}] Starting CSV generation...")
    
    # --- 解析可选命令行参数 ---
    target_count = None
    if len(sys.argv) > 1:
        try:
            target_count = int(sys.argv[1])
            if target_count <= 0:
                print("Fatal: The requested line count must be greater than 0.")
                sys.exit(1)
        except ValueError:
            print("Fatal: Invalid argument. Please provide a valid integer.")
            sys.exit(1)
    
    success_count = 0
    fail_count = 0

    try:
        # --- 先完整读取输入文件，获取精确的有效行数 ---
        with open(INPUT_FILE, 'r', encoding='utf-8') as f_in:
            # 自动过滤掉空行和没有逗号的无效数据行
            valid_lines = [line.strip() for line in f_in if line.strip() and ',' in line]
            
        total_valid_lines = len(valid_lines)
        
        # --- 核心越权拦截逻辑 ---
        if target_count is not None:
            if target_count > total_valid_lines:
                # 当请求行数大于实际行数时，直接英文报错退出
                print(f"Fatal: Requested {target_count} lines, but {INPUT_FILE} only contains {total_valid_lines} valid lines.")
                sys.exit(1)
            else:
                # 仅截取前 N 行进行后续处理
                lines_to_process = valid_lines[:target_count]
        else:
            # 如果没有入参，处理全量有效行
            lines_to_process = valid_lines

        # --- 遍历确定好的目标行，执行真实的鉴权请求 ---
        with open(OUTPUT_FILE, 'w', encoding='utf-8') as f_out:
            for line in lines_to_process:
                try:
                    # 1. 解析新格式: userid-name-password,original_ak,original_sk
                    parts = line.split(',')
                    auth_info = parts[0]
                    # 安全提取第二个字段 (Original AK)，用于C端桶名拼接
                    original_ak = parts[1].strip() if len(parts) > 1 else ""
                    
                    # 2. 从 auth_info 中提取 IAM 认证所需字段
                    u_id, u_name, u_pwd = auth_info.split('-', 2)
                except ValueError:
                    print(f"SKIP (Format error in line: {line[:20]}...)")
                    continue
                
                print(f"[*] Fetching STS for {u_name}...", end=" ", flush=True)
                
                # 3. 调用 API 获取临时凭证
                result, err = get_temporary_credentials(u_id, u_name, u_pwd)
                
                if result:
                    temp_ak, temp_sk, sts_token = result
                    # 4. 将 original_ak 作为第5列输出
                    # 最终格式: Username,TempAK,TempSK,SecurityToken,OriginalAK
                    f_out.write(f"{u_name},{temp_ak},{temp_sk},{sts_token},{original_ak}\n")
                    print("OK")
                    success_count += 1
                else:
                    print(f"FAILED ({err})")
                    fail_count += 1

    except FileNotFoundError:
        print(f"Fatal: {INPUT_FILE} not found.")
        sys.exit(1)

    print(f"\nSummary: {success_count} exported, {fail_count} failed.")
    print(f"Output saved to: {OUTPUT_FILE}")

if __name__ == "__main__":
    main() 
