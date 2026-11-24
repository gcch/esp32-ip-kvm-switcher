Import("env")

import os
from platformio.project.config import ProjectConfig


tool_dir = env.PioPlatform().get_package_dir("tool-mklittlefs")
if not tool_dir:
    tool_dir = os.path.join(
        ProjectConfig.get_instance().get("platformio", "packages_dir"),
        "tool-mklittlefs",
    )
tool_name = "mklittlefs.exe" if os.name == "nt" else "mklittlefs"
tool_path = os.path.join(tool_dir, tool_name)

if not os.path.isfile(tool_path):
    raise FileNotFoundError("PlatformIO mklittlefs executable not found: " + tool_path)

env.Replace(MKFSTOOL=tool_path)
