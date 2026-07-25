# 复用脚本：设置 ESP-IDF v6.0.2 环境并运行 idf.py，输出 tee 到 idf_out.log
# 用法： powershell -File run_idf.ps1 <idf.py 参数...>
$env:IDF_PATH="C:\esp\v6.0.2\esp-idf"
$env:ESP_IDF_VERSION="6.0.2"
$env:IDF_TOOLS_PATH="C:\Espressif\tools"
$env:IDF_PYTHON_ENV_PATH="C:\Espressif\tools\python\v6.0.2\venv"
$env:IDF_COMPONENT_LOCAL_STORAGE_URL="file://C:\Espressif\tools"
$env:IDF_CCACHE_ENABLE="1"
$toolPaths = @(
  "C:\Espressif\tools\ccache\4.12.1\ccache-4.12.1-windows-x86_64",
  "C:\Espressif\tools\cmake\4.0.3\bin",
  "C:\Espressif\tools\ninja\1.12.1\",
  "C:\Espressif\tools\idf-exe\1.0.3\",
  "C:\Espressif\tools\esp-rom-elfs\20241011\",
  "C:\Espressif\tools\xtensa-esp-elf-gdb\17.1_20260402\xtensa-esp-elf-gdb\bin",
  "C:\Espressif\tools\xtensa-esp-elf\esp-15.2.0_20251204\xtensa-esp-elf\bin",
  "C:\Espressif\tools\riscv32-esp-elf\esp-15.2.0_20251204\riscv32-esp-elf\bin",
  "C:\Espressif\tools\python\v6.0.2\venv\Scripts"
) -join ";"
$env:PATH = "$toolPaths;$env:PATH"
Set-Location "C:\Users\EDY\WorkBuddy\2026-07-11-09-29-19\voice-chatbot-firmware"
$py = "C:\Espressif\tools\python\v6.0.2\venv\Scripts\python.exe"
$log = "C:\Users\EDY\WorkBuddy\2026-07-11-09-29-19\voice-chatbot-firmware\idf_out.log"
"===== idf.py $args =====" | Out-File -FilePath $log -Encoding utf8
& $py "C:\esp\v6.0.2\esp-idf\tools\idf.py" @args *>&1 | Tee-Object -FilePath $log -Append
"===== EXIT CODE: $LASTEXITCODE =====" | Out-File -FilePath $log -Append -Encoding utf8
exit $LASTEXITCODE