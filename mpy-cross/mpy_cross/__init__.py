#!/usr/bin/env python3
#
# This file is part of the MicroPython project, http://micropython.org/
#
# The MIT License (MIT)
#
# Copyright (c) 2022 Andrew Leech
# Copyright (c) 2022 Jim Mussared
#
# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documentation files (the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of the Software, and to permit persons to whom the Software is
# furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included in
# all copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
# AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
# THE SOFTWARE.

from __future__ import print_function
import os
import re
import stat
import subprocess

NATIVE_ARCHS = {
    "NATIVE_ARCH_NONE": "",
    "NATIVE_ARCH_X86": "x86",
    "NATIVE_ARCH_X64": "x64",
    "NATIVE_ARCH_ARMV6": "armv6",
    "NATIVE_ARCH_ARMV6M": "armv6m",
    "NATIVE_ARCH_ARMV7M": "armv7m",
    "NATIVE_ARCH_ARMV7EM": "armv7em",
    "NATIVE_ARCH_ARMV7EMSP": "armv7emsp",
    "NATIVE_ARCH_ARMV7EMDP": "armv7emdp",
    "NATIVE_ARCH_XTENSA": "xtensa",
    "NATIVE_ARCH_XTENSAWIN": "xtensawin",
    "NATIVE_ARCH_RV32IMC": "rv32imc",
}

globals().update(NATIVE_ARCHS)

__all__ = ["version", "compile", "run", "CrossCompileError"] + list(NATIVE_ARCHS.keys())


class CrossCompileError(Exception):
    pass


_VERSION_RE = re.compile("mpy-cross emitting mpy v([0-9]+)(?:.([0-9]+))?")


def _find_mpy_cross_binary(mpy_cross):
    if mpy_cross:
        return mpy_cross
    return os.path.abspath(os.path.join(os.path.dirname(__file__), "../build/mpy-cross"))


def mpy_version(mpy_cross=None):
    """
    Get the version and sub-version of the .mpy file format generated by this version of mpy-cross.

    Returns: A tuple of `(mpy_version, mpy_sub_version)`
    Optional keyword arguments:
     - mpy_cross: Specific mpy-cross binary to use
    """
    version_info = run(["--version"], mpy_cross=mpy_cross)
    match = re.search(_VERSION_RE, version_info)
    mpy_version, mpy_sub_version = int(match.group(1)), int(match.group(2) or "0")
    return (
        mpy_version,
        mpy_sub_version,
    )


def compile(src, dest=None, src_path=None, opt=None, march=None, mpy_cross=None, extra_args=None):
    """
    Compile the specified .py file with mpy-cross.

    Returns: Standard output from mpy-cross as a string.

    Required arguments:
     - src:        The path to the .py file

    Optional keyword arguments:
     - dest:       The output .mpy file. Defaults to `src` (with .mpy extension)
     - src_path:   The path to embed in the .mpy file (defaults to `src`)
     - opt:        Optimisation level (0-3, default 0)
     - march:      One of the `NATIVE_ARCH_*` constants (defaults to NATIVE_ARCH_NONE)
     - mpy_cross:  Specific mpy-cross binary to use
     - extra_args: Additional arguments to pass to mpy-cross (e.g. `["-X", "emit=native"]`)
    """
    if not src:
        raise ValueError("src is required")
    if not os.path.exists(src):
        raise CrossCompileError("Input .py file not found: {}.".format(src))

    args = []

    if src_path:
        args += ["-s", src_path]

    if dest:
        args += ["-o", dest]

    if march:
        args += ["-march=" + march]

    if opt is not None:
        args += ["-O{}".format(opt)]

    if extra_args:
        args += extra_args

    args += [src]

    run(args, mpy_cross)


def run(args, mpy_cross=None):
    """
    Run mpy-cross with the specified command line arguments.
    Prefer to use `compile()` instead.

    Returns: Standard output from mpy-cross as a string.

    Optional keyword arguments:
     - mpy_cross: Specific mpy-cross binary to use
    """
    mpy_cross = _find_mpy_cross_binary(mpy_cross)

    if not os.path.exists(mpy_cross):
        raise CrossCompileError("mpy-cross binary not found at {}.".format(mpy_cross))

    try:
        st = os.stat(mpy_cross)
        os.chmod(mpy_cross, st.st_mode | stat.S_IEXEC)
    except OSError:
        pass

    try:
        return subprocess.check_output([mpy_cross] + args, stderr=subprocess.STDOUT).decode()
    except subprocess.CalledProcessError as er:
        raise CrossCompileError(er.output.decode())
