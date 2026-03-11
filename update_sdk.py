import os
import platform
import subprocess
import shutil
import zipfile
import urllib.request

def run_command(command, cwd=None, env=None):
    print(f"Running: {command} in {cwd or 'current directory'}")
    process = subprocess.Popen(command, shell=True, cwd=cwd, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    if process.stdout:
        for line in process.stdout:
            print(line, end='')
    process.wait()
    if process.returncode != 0:
        raise Exception(f"Command failed with return code {process.returncode}")

def main():
    base_dir = os.path.dirname(os.path.abspath(__file__))
    sdk_zip_url = "https://github.com/huaweicloud/huaweicloud-sdk-c-obs/archive/refs/heads/master.zip"
    sdk_zip_path = os.path.join(base_dir, "master_downloaded.zip")

    # 1. Detect architecture
    machine = platform.machine().lower()
    print(f"Detected architecture: {machine}")
    
    is_arm = "aarch64" in machine or "arm" in machine
    is_x86 = "x86_64" in machine or "amd64" in machine

    if not (is_arm or is_x86):
        print(f"Unknown architecture {machine}, defaulting to x86 patterns.")
        is_x86 = True

    # 2. Check for existing SDK or download
    existing_zip = os.path.join(base_dir, "master.zip")
    if os.path.exists(existing_zip):
        print(f"Found existing {existing_zip}, skipping download.")
        sdk_zip_path = existing_zip
    else:
        print(f"Downloading SDK from {sdk_zip_url}...")
        try:
            urllib.request.urlretrieve(sdk_zip_url, sdk_zip_path)
        except Exception as e:
            print(f"Download failed: {e}.")
            raise Exception("SDK download failed and no existing master.zip found.")

    # 3. Unzip
    print("Unzipping SDK...")
    temp_extract_dir = os.path.join(base_dir, "sdk_temp_extract")
    if os.path.exists(temp_extract_dir):
        shutil.rmtree(temp_extract_dir)
    os.makedirs(temp_extract_dir)
    
    with zipfile.ZipFile(sdk_zip_path, 'r') as zip_ref:
        zip_ref.extractall(temp_extract_dir)
    
    extracted_items = os.listdir(temp_extract_dir)
    if not extracted_items:
         raise Exception("Zip file is empty")
    
    actual_sdk_root = os.path.join(temp_extract_dir, extracted_items[0])
    print(f"Actual SDK root: {actual_sdk_root}")

    # 4. Navigate to build directory
    build_dir = os.path.join(actual_sdk_root, "source/eSDK_OBS_API/eSDK_OBS_API_C++")
    if not os.path.exists(build_dir):
         # Try alternative path just in case
         build_dir = os.path.join(actual_sdk_root, "eSDK_OBS_API/eSDK_OBS_API_C++")
         if not os.path.exists(build_dir):
              raise Exception(f"Could not find build directory in {actual_sdk_root}")
    
    # 5. Build SDK
    env = os.environ.copy()
    env["SPDLOG_VERSION"] = "spdlog-1.12.0"
    
    if is_x86:
        build_cmd = "bash build.sh sdk"
    else:
        build_cmd = "bash build_aarch.sh sdk"
    
    print(f"Building SDK for {'ARM' if is_arm else 'x86'}...")
    run_command(build_cmd, cwd=build_dir, env=env)

    # 6. Extract built SDK
    print("Extracting sdk.tgz...")
    run_command("tar zxvf sdk.tgz", cwd=build_dir)

    # 7. Replace lib
    local_lib_dir = os.path.join(base_dir, "lib")
    sdk_lib_dir = os.path.join(build_dir, "lib")
    
    if not os.path.exists(sdk_lib_dir):
         potential_lib = os.path.join(build_dir, "sdk", "lib")
         if os.path.exists(potential_lib):
              sdk_lib_dir = potential_lib

    print(f"Replacing {local_lib_dir} with {sdk_lib_dir}...")
    if os.path.exists(local_lib_dir):
        shutil.rmtree(local_lib_dir)
    shutil.copytree(sdk_lib_dir, local_lib_dir)

    # 8. Compile tool
    print("Compiling tool...")
    run_command("make clean && make all", cwd=base_dir)

    # Cleanup
    print("Cleaning up temporary files...")
    shutil.rmtree(temp_extract_dir)
    if os.path.exists(sdk_zip_path) and sdk_zip_path.endswith("master_downloaded.zip"):
         os.remove(sdk_zip_path)

    print("Successfully updated SDK and compiled the tool.")

if __name__ == "__main__":
    main()
