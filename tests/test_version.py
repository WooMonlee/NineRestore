#!/usr/bin/env python3
"""version.py 的单元测试（零依赖）。跑法：python tests/test_version.py

命名规范（2026-11-07）：
  测试版 1.0 beta.N → 内部 1.0.0.N → 文件 九转大还原1.0-beta.N.exe
  正式版 1.0.0     → 内部 1.0.0.0 → 文件 九转大还原1.0.0.exe
  正式后 1.0.1     → 内部 1.0.1.0 → 文件 九转大还原1.0.1.exe
"""
import importlib.util
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
_spec = importlib.util.spec_from_file_location("zjversion", ROOT / "tools" / "version.py")
v = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(v)

_checks = 0
_fails = 0


def check(cond, msg):
    global _checks, _fails
    _checks += 1
    if not cond:
        _fails += 1
        print("  [FAIL] " + msg)


def eq(a, b, msg):
    check(a == b, "%s -> %r != %r" % (msg, a, b))


def macros(major, minor, patch, beta):
    return {
        "SYSRECOVER_MAJOR": major,
        "SYSRECOVER_MINOR": minor,
        "SYSRECOVER_PATCH": patch,
        "SYSRECOVER_BETA": beta,
        "SYSRECOVER_VERSION": "%d.%d.%d.%d" % (major, minor, patch, beta),
        "SYSRECOVER_DISPLAY": "",
    }


# ---- 派生规则 ----
d = v.derive(macros(1, 0, 0, 2))
eq(d["internal"], "1.0.0.2", "beta.2 internal")
eq(d["display"], "1.0 beta.2", "beta.2 display")
eq(d["filename"], "九转大还原1.0-beta.2.exe", "beta.2 filename")

d = v.derive(macros(1, 0, 0, 0))
eq(d["internal"], "1.0.0.0", "正式版 internal")
eq(d["display"], "1.0.0", "正式版 display")
eq(d["filename"], "九转大还原1.0.0.exe", "正式版 filename")

d = v.derive(macros(1, 0, 1, 0))
eq(d["internal"], "1.0.1.0", "正式后 1.0.1 internal")
eq(d["display"], "1.0.1", "正式后 1.0.1 display")
eq(d["filename"], "九转大还原1.0.1.exe", "正式后 1.0.1 filename")

# 已知副作用：正式版四段会小于最后一个 beta（0 回落），仅影响观感
check(int(v.derive(macros(1, 0, 0, 0))["internal"].split(".")[3]) <
      int(v.derive(macros(1, 0, 0, 9))["internal"].split(".")[3]),
      "正式版第四段应回落到 0（1.0.0.9 → 1.0.0.0）")

# ---- 一致性校验 ----
good = macros(1, 0, 0, 2)
good["SYSRECOVER_VERSION"] = "1.0.0.2"
good["SYSRECOVER_DISPLAY"] = "1.0 beta.2"
eq(v.check(good), [], "一致性通过")

bad = dict(good)
bad["SYSRECOVER_DISPLAY"] = "1.0.0.2"
check(len(v.check(bad)) > 0, "显示串写错必须报错")

bad2 = macros(1, 0, 1, 2)  # beta 期间第三位不为 0
bad2["SYSRECOVER_VERSION"] = "1.0.1.2"
bad2["SYSRECOVER_DISPLAY"] = "1.0 beta.2"
check(len(v.check(bad2)) > 0, "beta 期间第三位必须为 0")

# ---- 读写往返（临时文件）----
with tempfile.TemporaryDirectory() as td:
    p = Path(td) / "version.h"
    p.write_text(
        "#pragma once\n"
        "#define SYSRECOVER_MAJOR 1\n"
        "#define SYSRECOVER_MINOR 0\n"
        "#define SYSRECOVER_PATCH 0\n"
        "#define SYSRECOVER_BETA 2\n"
        '#define SYSRECOVER_VERSION "1.0.0.2"\n'
        '#define SYSRECOVER_DISPLAY "1.0 beta.2"\n',
        encoding="utf-8", newline="\n")
    m = v.read_macros(p)
    eq(m["SYSRECOVER_BETA"], 2, "read_macros beta")
    m["SYSRECOVER_BETA"] = 3
    v.write_macros(m, p)
    m2 = v.read_macros(p)
    eq(m2["SYSRECOVER_BETA"], 3, "write_macros 往返 beta")
    eq(m2["SYSRECOVER_VERSION"], "1.0.0.3", "write_macros 自动同步内部四段")
    eq(m2["SYSRECOVER_DISPLAY"], "1.0 beta.3", "write_macros 自动同步显示串")
    raw = p.read_bytes()
    check(b"\r\n" not in raw, "version.h 不应写成 CRLF")

print("%d checks, %d failures" % (_checks, _fails))
sys.exit(1 if _fails else 0)
