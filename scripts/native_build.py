"""Build the freestanding C test harnesses with the host's native compiler."""
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys


class NativeCompiler:
    def __init__(self):
        self.windows = sys.platform == "win32"
        if self.windows:
            vswhere = Path("C:/Program Files (x86)/Microsoft Visual Studio/Installer/vswhere.exe")
            found = subprocess.check_output([str(vswhere), "-products", "*", "-find",
                r"VC\Tools\MSVC\**\bin\Hostx64\x64\cl.exe"], text=True).splitlines()
            if not found:
                raise RuntimeError("Install the Visual Studio C++ build tools to run native tests")
            self.compiler = Path(found[-1])
        else:
            self.command = shlex.split(os.environ.get("CC", "gcc"))
            if not self.command or not shutil.which(self.command[0]):
                raise RuntimeError("Install a native C compiler (gcc), or set CC")

    def compile_command(self, source, output, *, includes=(), defines=(),
                        disabled_warnings=(), force_include=()):
        if self.windows:
            command = [str(self.compiler), "/nologo", "/c", "/std:c11", "/W4", "/WX", "/GS-", "/Oi-", "/Zl"]
            command += ["/wd" + str(number) for number in disabled_warnings]
            command += ["/I" + str(path) for path in includes]
            command += ["/I" + str(self.compiler.parents[3] / "include")]
            command += ["/D" + define for define in defines]
            command += ["/FI" + str(path) for path in force_include]
            return [*command, str(source), "/Fo" + str(output)]
        # Keep test memory primitives local to each library. Otherwise ELF symbol
        # interposition can redirect the fixtures' fake malloc/stdio to libc.
        command = [*self.command, "-c", "-std=c11", "-Wall", "-Wextra", "-Werror",
                   "-fPIC", "-ffreestanding", "-fno-builtin", "-fno-stack-protector",
                   "-Wno-unused-parameter", "-Wno-unused-function", "-Wno-sign-compare",
                   "-Wno-unused-variable", "-Wno-unused-value", "-Wno-misleading-indentation",
                   "-Wno-missing-field-initializers", "-include",
                   str(Path(__file__).resolve().parents[1] / "tests/native/include/native_compat.h")]
        command += ["-I" + str(path) for path in includes]
        command += ["-D" + define for define in defines]
        # The SDK HAL deliberately writes host pointers into 32-bit fake MMIO
        # registers. Match the existing MSVC suppressions only in those tests.
        if any(number in disabled_warnings for number in (4302, 4311)):
            command += ["-Wno-pointer-to-int-cast"]
        if 4312 in disabled_warnings:
            command += ["-Wno-int-to-pointer-cast"]
        for path in force_include:
            command += ["-include", str(path)]
        return [*command, str(source), "-o", str(output)]

    def link_command(self, output, objects, *, stack_probes=False, exports=()):
        if self.windows:
            command = [str(self.compiler.parent / "link.exe"), "/nologo", "/dll", "/noentry", "/nodefaultlib",
                       "/out:" + str(output), *map(str, objects)]
            command += ["/export:" + name for name in exports]
            if stack_probes:
                command += [str(self.compiler.parents[3] / "lib/x64/libcmt.lib")]
            return command
        return [*self.command, "-shared", "-nostdlib", "-Wl,-Bsymbolic", "-Wl,-z,defs",
                "-o", str(output), *map(str, objects)]


def sdk_path(root):
    """Use an activated SDK, or the existing shared Windows installation."""
    if os.environ.get("IDF_PATH"):
        return Path(os.environ["IDF_PATH"])
    version = json.loads((root / "toolchain.json").read_text())["espIdfVersion"]
    local = root / ".tools/esp" / version / "esp-idf"
    if local.is_dir():
        return local
    common = Path(subprocess.check_output(["git", "-C", str(root), "rev-parse",
        "--path-format=absolute", "--git-common-dir"], text=True).strip()).parent
    return common / ".tools/esp" / version / "esp-idf"
