"""Run the actual sensor driver against a simulated I2C device, without hardware."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile


root = Path(__file__).resolve().parents[1]
headers = root / "firmware/.pio/libdeps/esp32_s3_box_3/ArduinoJson/src"
if not (headers / "ArduinoJson.h").is_file():
    raise SystemExit("Build the esp32_s3_box_3 firmware first to install its pinned ArduinoJson headers.")

env = {key.upper(): value for key, value in os.environ.items()} if os.name == "nt" else dict(os.environ)
if os.name == "nt":
    if not shutil.which("cl.exe"):
        vswhere = Path(env["PROGRAMFILES(X86)"]) / "Microsoft Visual Studio/Installer/vswhere.exe"
        installation = subprocess.check_output([str(vswhere), "-latest", "-products", "*", "-requires",
                                                "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
                                                "-property", "installationPath"], text=True).strip()
        devcmd = Path(installation) / "Common7/Tools/VsDevCmd.bat"
        if not installation or not devcmd.is_file():
            raise SystemExit("Visual Studio C++ build tools are required.")
        with tempfile.TemporaryDirectory(prefix="speaker-compiler-env-") as temporary:
            batch = Path(temporary) / "environment.cmd"
            batch.write_text(f'@echo off\ncall "{devcmd}" -arch=x64 -host_arch=x64 >nul\n'
                             'if errorlevel 1 exit /b %errorlevel%\nset\n')
            result = subprocess.run(["cmd.exe", "/d", "/c", batch.name], cwd=temporary,
                                    env=env, capture_output=True, text=True)
            if result.returncode:
                raise SystemExit("Visual Studio environment setup failed: " + result.stderr)
        for line in result.stdout.splitlines():
            key, separator, value = line.partition("=")
            if separator and key:
                env[key.upper()] = value
    compiler = shutil.which("cl.exe", path=env["PATH"])
    if not compiler:
        raise SystemExit("Could not locate the Visual Studio compiler.")
else:
    compiler = shutil.which("c++")
    if not compiler:
        raise SystemExit("A C++ compiler is required.")

includes = [root / "tests/sensors", root / "firmware/include", headers]
sources = [root / "tests/sensors/sensor_test.cpp", root / "tests/sensors/presence_test.cpp", root / "firmware/src/board_sensors.cpp"]
with tempfile.TemporaryDirectory(prefix="speaker-sensor-tests-") as temporary:
    env["TMP"] = env["TEMP"] = temporary
    for target in ("BOARD_ESP32_S3_BOX_3", "BOARD_MUSE_LUXE"):
        executable = Path(temporary) / (target + (".exe" if os.name == "nt" else ""))
        if os.name == "nt":
            command = [compiler, "/nologo", "/std:c++17", "/EHsc", "/W3", "/D" + target,
                       *["/I" + str(path) for path in includes], *map(str, sources), "/Fe:" + str(executable)]
        else:
            command = [compiler, "-std=c++17", "-Wall", "-Wextra", "-D" + target,
                       *["-I" + str(path) for path in includes], *map(str, sources), "-o", str(executable)]
        subprocess.run(command, cwd=temporary, env=env, check=True)
        subprocess.run([str(executable)], cwd=temporary, env=env, check=True)
