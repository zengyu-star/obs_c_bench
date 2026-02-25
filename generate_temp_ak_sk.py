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
        
        # [修复]: 返回一个包含三个元素的元组，以及一个 None 作为 error 占位符
        return (sts_data.get('access'), sts_data.get('secret'), sts_data.get('securitytoken')), None

    except requests.exceptions.RequestException as e:
        return None, f"NET_ERR: {str(e)}"

def main():
    print(f"[{datetime.now().strftime('%H:%M:%S')}] Starting CSV generation...")
    
    success_count = 0
    fail_count = 0

    try:
        with open(INPUT_FILE, 'r', encoding='utf-8') as f_in, \
             open(OUTPUT_FILE, 'w', encoding='utf-8') as f_out:
            
            for line in f_in:
                line = line.strip()
                if not line or ',' not in line:
                    continue
                
                # --- 仅包裹解析异常，防止吞咽下游的网络/解包异常 ---
                try:
                    # 1. 解析新格式: userid-name-password,ak,sk
                    parts = line.split(',')
                    auth_info = parts[0]
                    
                    # 2. 从 auth_info 中提取 IAM 认证所需字段 (按照 '-' 分隔前3项)
                    u_id, u_name, u_pwd = auth_info.split('-', 2)
                except ValueError:
                    print(f"SKIP (Format error in line: {line[:20]}...)")
                    continue
                
                print(f"[*] Fetching STS for {u_name}...", end=" ", flush=True)
                
                # 3. 调用 API 获取临时凭证
                result, err = get_temporary_credentials(u_id, u_name, u_pwd)
                
                if result:
                    temp_ak, temp_sk, sts_token = result
                    # 4. 将 username 作为第一列输出，补全租户上下文
                    # 最终 temptoken.dat 格式: Username,TempAK,TempSK,SecurityToken
                    f_out.write(f"{u_name},{temp_ak},{temp_sk},{sts_token}\n")
                    print("OK")
                    success_count += 1
                else:
                    print(f"FAILED ({err})")
                    fail_count += 1

    except FileNotFoundError:
        print(f"Fatal: {INPUT_FILE} not found.")
        return

    print(f"\nSummary: {success_count} exported, {fail_count} failed.")
    print(f"Output saved to: {OUTPUT_FILE}")

if __name__ == "__main__":
    main()
