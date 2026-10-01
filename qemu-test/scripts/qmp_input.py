#!/usr/bin/env python3
"""通过 QEMU QMP 给 WinCE 虚拟机发精确的鼠标/键盘事件（headless 脚本化操作）。

前置: 容器内 QEMU 以 -qmp tcp:0.0.0.0:4445 启动，并映射到宿主机（如 14445）。

动作序列（命令行逗号分隔，按顺序执行）:
  move,X,Y        绝对移动鼠标到屏幕像素 (X,Y)
  click,X,Y       移动并左键单击
  dblclick,X,Y    移动并左键双击
  btn,left|right  按下并松开
  key,<qcode>     按键，如 key,ret / key,ctrl-esc / key,tab
  text,<str>      逐字符输入（限 ASCII）
  wait,<sec>      等待

示例:
  python3 qmp_input.py --port 14445 move,20,584 click,20,584 text,"\\navit\\navit.exe" key,ret

用法: python3 qmp_input.py [--host H] [--port P] [--w W] [--h H] action,args action,args ...
"""
import socket, json, sys, time

QW = 32767

# ASCII -> QEMU qcode（覆盖常用字符）
QCODE = {
    " ": "spc", ".": "dot", ",": "comma", "/": "slash", "\\": "backslash",
    ":": "shift-semicolon", ";": "semicolon", "-": "minus", "=": "equal",
    "_": "shift-minus", "'": "apostrophe", '"': "shift-apostrophe",
    "!": "shift-1", "@": "shift-2", "#": "shift-3", "$": "shift-4",
    "%": "shift-5", "^": "shift-6", "&": "shift-7", "*": "shift-8",
    "(": "shift-9", ")": "shift-0",
}
for c in "abcdefghijklmnopqrstuvwxyz":
    QCODE[c] = c
    QCODE[c.upper()] = "shift-" + c
for d in "0123456789":
    QCODE[d] = d


class Qmp:
    def __init__(self, host, port):
        self.s = socket.create_connection((host, port), timeout=15)
        self.s.settimeout(15)
        self.f = self.s.makefile("rwb")
        self._recv()                       # greeting
        self.cmd({"execute": "qmp_capabilities"})

    def _recv(self):
        line = self.f.readline()
        if not line:
            raise EOFError("QMP closed")
        return json.loads(line)

    def cmd(self, obj):
        self.f.write((json.dumps(obj) + "\n").encode())
        self.f.flush()
        while True:
            r = self._recv()
            if "event" in r:
                continue
            return r

    def events(self, evs):
        self.cmd({"execute": "input-send-event", "arguments": {"events": evs}})

    def move(self, x, y, w, h):
        ax = int(x / w * QW)
        ay = int(y / h * QW)
        self.events([
            {"type": "abs", "data": {"axis": "x", "value": ax}},
            {"type": "abs", "data": {"axis": "y", "value": ay}},
        ])

    def btn(self, down, button="left"):
        self.events([{"type": "btn", "data": {"down": down, "button": button}}])

    def click(self, x, y, w, h):
        self.move(x, y, w, h)
        time.sleep(0.15)
        self.btn(True); time.sleep(0.06); self.btn(False)

    def dblclick(self, x, y, w, h):
        self.move(x, y, w, h)
        time.sleep(0.15)
        for _ in range(2):
            self.btn(True); time.sleep(0.05); self.btn(False); time.sleep(0.08)

    def key(self, qcode):
        for ev in (True, False):
            self.events([{"type": "key", "data": {"down": ev, "key": {"type": "qcode", "data": qcode}}}])


def main():
    host, port, w, h = "127.0.0.1", 14445, 1024, 602
    args = sys.argv[1:]
    actions = []
    i = 0
    while i < len(args):
        a = args[i]
        if a == "--host":
            host = args[i+1]; i += 2; continue
        if a == "--port":
            port = int(args[i+1]); i += 2; continue
        if a == "--w":
            w = int(args[i+1]); i += 2; continue
        if a == "--h":
            h = int(args[i+1]); i += 2; continue
        actions.append(a); i += 1
    q = Qmp(host, port)
    print("[*] connected QMP %s:%d  screen %dx%d" % (host, port, w, h))
    for a in actions:
        parts = a.split(",", 2)
        op = parts[0]
        if op in ("move", "click", "dblclick"):
            x, y = int(parts[1]), int(parts[2])
            {"move": q.move, "click": q.click, "dblclick": q.dblclick}[op](x, y, w, h)
            print("  %s %d,%d" % (op, x, y))
        elif op == "btn":
            q.btn(True, parts[1]); q.btn(False, parts[1])
            print("  btn %s" % parts[1])
        elif op == "key":
            q.key(parts[1])
            print("  key %s" % parts[1])
        elif op == "combo":
            keys = parts[1].split("-")
            for k in keys[:-1]:
                q.events([{"type": "key", "data": {"down": True, "key": {"type": "qcode", "data": k}}}])
                time.sleep(0.05)
            last = keys[-1]
            q.events([{"type": "key", "data": {"down": True, "key": {"type": "qcode", "data": last}}}])
            time.sleep(0.05)
            q.events([{"type": "key", "data": {"down": False, "key": {"type": "qcode", "data": last}}}])
            for k in reversed(keys[:-1]):
                q.events([{"type": "key", "data": {"down": False, "key": {"type": "qcode", "data": k}}}])
                time.sleep(0.05)
            print("  combo %s" % parts[1])
        elif op == "text":
            txt = a.split(",", 1)[1]
            for ch in txt:
                qc = QCODE.get(ch)
                if qc:
                    q.key(qc); time.sleep(0.03)
            print("  text %r" % txt)
        elif op == "wait":
            time.sleep(float(parts[1]))
            print("  wait %s" % parts[1])
        else:
            print("  ?? unknown action %r" % a)
        time.sleep(0.1)
    print("[*] done")


if __name__ == "__main__":
    main()
