#!/usr/bin/env python3
"""Mechanical check of AGENTS.md invariant 2: every vkQueue* driver call holds the
queue's QueueState mutex, which we guarantee by letting only gpu/queue.cc call the
driver. Everything else goes through QueueTable (queues.submit / present / ...).

usage: check_queue_calls.py SRC_ROOT      scan SRC_ROOT/**/*.{cc,hh}
       check_queue_calls.py --self-test   prove the detectors fire

Scanned text has comments, string literals and char literals blanked, so names in
intercept tables ("vkQueueSubmit"), hook definitions (VkHook_QueuePresentKHR) and
prose never match. Skipped trees: vendor/ (imgui's backend routes its vkQueue*
through the layer callback, see overlay_ui.cc) and tests/.

What counts as a raw call (CALL rules; allowed only in gpu/queue.cc):
  * vkQueueSubmit(...), vkQueueSubmit2[KHR], vkQueueBindSparse, vkQueuePresentKHR,
    vkQueueWaitIdle, vkQueue{Begin,End,Insert}DebugUtilsLabelEXT called by name
  * dispatch-table members:  x.QueueSubmit(  x->QueuePresentKHR(  vkd.QueueWaitIdle(
  * QueueTable::Fns members: fn.submit(  fn.present(  fn.waitIdle(  ...
  * the per-queue real-present pointer: state.realPresent(  p->realPresent(
What counts as a raw pointer (TYPE rule): any mention of PFN_vkQueue*. A stored
driver pointer is how a bypass gets called later.
"""
import os
import re
import sys

NAMES = (r"(?:Submit2KHR|Submit2|Submit|BindSparse|PresentKHR|WaitIdle|"
         r"BeginDebugUtilsLabelEXT|EndDebugUtilsLabelEXT|InsertDebugUtilsLabelEXT)")
FN_MEMBERS = r"(?:submit2KHR|submit2|submit|bindSparse|present|waitIdle|beginLabel|endLabel|insertLabel)"

CALL_RULES = [
    ("direct vkQueue* call", re.compile(r"\bvkQueue" + NAMES + r"\s*\(")),
    ("dispatch-table Queue* call", re.compile(r"(?:\.|->)\s*Queue" + NAMES + r"\s*\(")),
    ("QueueTable fn.* call", re.compile(r"\bfn\s*(?:\.|->)\s*" + FN_MEMBERS + r"\s*\(")),
    ("realPresent pointer call", re.compile(r"(?:\.|->)\s*realPresent\s*\(")),
]
TYPE_RULES = [
    ("PFN_vkQueue* pointer", re.compile(r"\bPFN_vkQueue\w*")),
]

# Paths relative to the source root, '/'-separated.
# Only gpu/queue.cc may call the driver: it is the one place that takes the
# QueueState mutex around every call.
CALL_ALLOWED = {"gpu/queue.cc"}
# Declaring driver pointers is allowed where they are stored or plumbed:
#  gpu/queue.hh      QueueTable::Fns members and QueueState::realPresent
#  gpu/queue.cc      loads them
#  render/vkfuncs.hh, render/vkdispatch.*  generic dispatch-table declarations/loaders
TYPE_ALLOWED = {"gpu/queue.hh", "gpu/queue.cc", "render/vkfuncs.hh",
                "render/vkdispatch.hh", "render/vkdispatch.cc"}
SKIP_DIRS = {"vendor", "tests"}
# Baseline of known, not-yet-fixed violations: exact violation count per file.
# More than the baseline fails, fewer fails too (delete the entry once fixed).
KNOWN_VIOLATIONS = {}


def strip(src):
    """Blank comments and string/char literals, keeping offsets and newlines."""
    out = []
    i, n = 0, len(src)
    while i < n:
        c = src[i]
        two = src[i:i + 2]
        if two == "//":
            while i < n and src[i] != "\n":
                # a trailing backslash continues a line comment
                if src[i] == "\\" and i + 1 < n and src[i + 1] == "\n":
                    out.append(" \n"); i += 2; continue
                out.append(" "); i += 1
        elif two == "/*":
            j = src.find("*/", i + 2)
            j = n if j < 0 else j + 2
            out.append("".join("\n" if ch == "\n" else " " for ch in src[i:j])); i = j
        elif c == 'R' and src[i + 1:i + 2] == '"' and re.match(r'R"([^()\\ ]{0,16})\(', src[i:]):
            delim = re.match(r'R"([^()\\ ]{0,16})\(', src[i:]).group(1)
            end = ")" + delim + '"'
            j = src.find(end, i)
            j = n if j < 0 else j + len(end)
            out.append("".join("\n" if ch == "\n" else " " for ch in src[i:j])); i = j
        elif c in "\"'":
            j = i + 1
            while j < n and src[j] != c and src[j] != "\n":
                j += 2 if src[j] == "\\" else 1
            j = min(j + 1, n)
            out.append(c + "".join("\n" if ch == "\n" else " " for ch in src[i + 1:j - 1]) + (c if j - 1 > i else ""))
            i = j
        else:
            out.append(c); i += 1
    return "".join(out)


def scan_text(rel, src):
    """Return [(line, rule, text)] violations for one file."""
    text = strip(src)
    lines = src.splitlines()
    found = []
    rules = []
    if rel not in CALL_ALLOWED:
        rules += CALL_RULES
    if rel not in TYPE_ALLOWED and rel not in CALL_ALLOWED:
        rules += TYPE_RULES
    for name, rx in rules:
        for m in rx.finditer(text):
            line = text.count("\n", 0, m.start()) + 1
            found.append((line, name, lines[line - 1].strip() if line <= len(lines) else ""))
    return sorted(found)


def scan_tree(root):
    bad = []
    count = 0
    for dirpath, dirnames, files in os.walk(root):
        rel_dir = os.path.relpath(dirpath, root).replace(os.sep, "/")
        if rel_dir == ".":
            dirnames[:] = [d for d in dirnames if d not in SKIP_DIRS]
        for f in files:
            if not f.endswith((".cc", ".hh")):
                continue
            path = os.path.join(dirpath, f)
            rel = os.path.relpath(path, root).replace(os.sep, "/")
            count += 1
            with open(path, encoding="utf-8", errors="replace") as fh:
                for line, rule, text in scan_text(rel, fh.read()):
                    bad.append((rel, path, line, rule, text))
    return count, bad


def judge(bad, known=None):
    """bad: [(rel, path, line, rule, text)] -> (failures, notices) after the baseline."""
    known = KNOWN_VIOLATIONS if known is None else known
    by_file = {}
    for item in bad:
        by_file.setdefault(item[0], []).append(item)
    failures, notices = [], []
    for rel, items in sorted(by_file.items()):
        allowed = known.get(rel, 0)
        if len(items) == allowed:
            notices.append("%s: %d known violation(s) (follow-up: route through QueueTable)" % (rel, allowed))
            continue
        if allowed and len(items) < allowed:
            continue  # reported below as stale baseline
        for _, path, line, rule, text in items:
            failures.append("%s:%d: %s outside gpu/queue.cc: %s" % (path, line, rule, text))
        if allowed:
            failures.append("%s: %d violations, baseline allows only %d" % (rel, len(items), allowed))
    for rel, allowed in sorted(known.items()):
        have = len(by_file.get(rel, ()))
        if have < allowed:
            failures.append("%s: only %d violation(s) left, baseline says %d: delete or lower its KNOWN_VIOLATIONS entry"
                            % (rel, have, allowed))
    return failures, notices


def self_test():
    violations = {
        "direct": "void f(){ vkQueueSubmit(q, 1, &s, f); }",
        "direct submit2khr": "auto r = vkQueueSubmit2KHR (q,1,&s,f);",
        "present": "return vkQueuePresentKHR(q, &p);",
        "waitidle": "vkQueueWaitIdle(q);",
        "label": "vkQueueInsertDebugUtilsLabelEXT(q,&l);",
        "dispatch dot": "vkd.QueueSubmit(q,1,&s,f);",
        "dispatch arrow": "dev->QueuePresentKHR(q,&p);",
        "dispatch bindsparse split": "t.QueueBindSparse\n  (q,1,&b,f);",
        "fn member": "return fn.submit(queue, 1, &s, f);",
        "fn present": "fn->present(queue, &info)",
        "realPresent ptr": "state.realPresent(queue, info);",
        "realPresent arrow": "s->realPresent(queue, info);",
        "after comment": "/* ok */ vkQueueSubmit(q,1,&s,f); // x",
        "pfn type": "PFN_vkQueueSubmit raw = nullptr;",
    }
    clean = {
        "line comment": "// vkQueueSubmit(q, 1, &s, f);",
        "block comment": "/* vkQueuePresentKHR(q,&p); */ int x;",
        "string": 'strcmp(n, "vkQueueSubmit") == 0; log("vkQueueSubmit(passthrough)");',
        "hook def": "VKAPI_ATTR VkResult VKAPI_CALL VkHook_QueuePresentKHR(VkQueue q, const VkPresentInfoKHR* p)",
        "intercept table": "VKHOOK_INTERCEPT(Unconditional, QueuePresentKHR),",
        "wrapper": "queues.submit(q, 1, &s, f); logicalDevice->queues.present(q, p);",
        "escaped quote": r'puts("a\" vkQueueSubmit(x)"); int ok;',
        "raw string": 'auto s = R"x(vkQueueSubmit(q))x";',
        "continued comment": "// foo \\\nvkQueueSubmit(q);",
    }
    failures = []
    for name, snippet in violations.items():
        if not scan_text("present/x.cc", snippet):
            failures.append("missed violation: " + name)
    for name, snippet in clean.items():
        hits = scan_text("present/x.cc", snippet)
        if hits:
            failures.append("false positive: %s -> %s" % (name, hits))
    if scan_text("gpu/queue.cc", violations["direct"]):
        failures.append("gpu/queue.cc must be allowed to call")
    if scan_text("gpu/queue.hh", violations["pfn type"]):
        failures.append("gpu/queue.hh must be allowed to declare PFN_vkQueue*")
    if not scan_text("gpu/queue.hh", violations["direct"]):
        failures.append("gpu/queue.hh must not be allowed to call")
    if not scan_text("render/vkdispatch.cc", violations["dispatch dot"]):
        failures.append("render/vkdispatch.cc must not be allowed to call")
    def fake(rel, n):
        return [(rel, rel, i + 1, "r", "t") for i in range(n)]
    base = {"a.cc": 2}
    f, n = judge(fake("a.cc", 2), base)
    if f or not n:
        failures.append("baseline: exact count must pass with a notice")
    if not judge(fake("a.cc", 3), base)[0]:
        failures.append("baseline: more than the baseline must fail")
    if not judge(fake("a.cc", 2) + fake("b.cc", 1), base)[0]:
        failures.append("baseline: violation in another file must fail")
    if not any("delete" in x for x in judge(fake("a.cc", 1), base)[0]):
        failures.append("baseline: fewer than the baseline must fail asking to delete the entry")
    if not any("delete" in x for x in judge([], base)[0]):
        failures.append("baseline: fixed file must fail asking to delete the entry")
    if failures:
        print("check_queue_calls self-test FAILED:\n  " + "\n  ".join(failures), file=sys.stderr)
        return 1
    print("check_queue_calls self-test: %d violations detected, %d clean snippets pass"
          % (len(violations), len(clean)))
    return 0


def main(argv):
    if len(argv) == 2 and argv[1] == "--self-test":
        return self_test()
    if len(argv) != 2:
        print(__doc__, file=sys.stderr)
        return 2
    root = argv[1]
    if not os.path.isdir(root):
        print("not a directory: " + root, file=sys.stderr)
        return 2
    count, bad = scan_tree(root)
    if count == 0:
        print("no sources found under " + root, file=sys.stderr)
        return 2
    failures, notices = judge(bad)
    for n in notices:
        print(n)
    for f in failures:
        print(f, file=sys.stderr)
    if failures:
        print("%d problem(s) (AGENTS.md invariant 2: all vkQueue* calls go through QueueTable)" % len(failures),
              file=sys.stderr)
        return 1
    print("check_queue_calls: %d files, no new raw vkQueue* calls outside gpu/queue.cc" % count)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
