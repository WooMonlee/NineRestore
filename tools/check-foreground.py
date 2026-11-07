#!/usr/bin/env python3
"""N19 static guard: main-window foreground handling (see docs/34 §3.5).

Rules:
  1. GUI code must not create message boxes / shell opens without a real owner window.
  2. The three modal dialogs must restore the owner via RestoreForeground(owner), not
     a bare SetForegroundWindow(owner) whose return value is ignored.
  3. ui_foreground.cpp must keep the three-step fallback (SetForegroundWindow ->
     AttachThreadInput retry -> BringWindowToTop/SetWindowPos) and log a failure.
  4. main_win.cpp startup path must use RestoreForeground.
"""
import io
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GUI = os.path.join(ROOT, "src", "gui")
DIALOGS = ("confirm_dlg.cpp", "passwd_dlg.cpp", "initdisk_dlg.cpp")

BAD_OWNER_MSGBOX = re.compile(r"MessageBoxW\s*\(\s*(nullptr|NULL)\s*,")
BAD_OWNER_SHELL = re.compile(r"ShellExecuteW\s*\(\s*(nullptr|NULL)\s*,")
BARE_FOREGROUND = re.compile(r"SetForegroundWindow\s*\(\s*owner\s*\)")


def read(name):
    with io.open(os.path.join(GUI, name), encoding="utf-8") as f:
        return f.read()


def line_of(text, pos):
    return text.count("\n", 0, pos) + 1


def main():
    errs = []

    for name in sorted(os.listdir(GUI)):
        if not name.endswith((".cpp", ".h")):
            continue
        text = read(name)
        for pat, why in ((BAD_OWNER_MSGBOX, "MessageBoxW without owner"),
                         (BAD_OWNER_SHELL, "ShellExecuteW without owner")):
            for m in pat.finditer(text):
                errs.append("%s:%d %s (pass the main window)" %
                            (name, line_of(text, m.start()), why))

    for name in DIALOGS:
        text = read(name)
        if "RestoreForeground(owner)" not in text:
            errs.append("%s missing RestoreForeground(owner)" % name)
        for m in BARE_FOREGROUND.finditer(text):
            errs.append("%s:%d bare SetForegroundWindow(owner) "
                        "(return value ignored; use RestoreForeground)" %
                        (name, line_of(text, m.start())))

    fg = read("ui_foreground.cpp")
    for token in ("SetForegroundWindow(owner)", "AttachThreadInput",
                  "BringWindowToTop", "LogInfo"):
        if token not in fg:
            errs.append("ui_foreground.cpp lost fallback step: %s" % token)

    mw = read("main_win.cpp")
    if "RestoreForeground(h)" not in mw:
        errs.append("main_win.cpp startup path must call RestoreForeground(h)")

    if errs:
        sys.stderr.write("check-foreground: %d problem(s)\n" % len(errs))
        for e in errs:
            sys.stderr.write("  " + e + "\n")
        return 1
    print("check-foreground: ok (owner + foreground fallback)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
