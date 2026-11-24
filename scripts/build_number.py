Import("env")

from datetime import datetime


build_number = datetime.now().strftime("%Y%m%d-%H%M%S")
env.Append(CPPDEFINES=[("BUILD_NUMBER", '\\"' + build_number + '\\"')])
