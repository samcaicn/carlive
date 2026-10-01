#!/usr/bin/env python3
"""比较两张 PPM 的差异（用于无图形环境下量化判断屏幕是否变化）。"""
import sys


def load(ppm):
    d = open(ppm, "rb").read()
    assert d[:2] == b"P6"
    i, fields = 2, []
    while len(fields) < 3:
        while d[i] in b" \t\r\n":
            i += 1
        if d[i] == ord("#"):
            while d[i] != ord("\n"):
                i += 1
            continue
        s = i
        while d[i] not in b" \t\r\n":
            i += 1
        fields.append(int(d[s:i]))
    w, h, mx = fields
    while d[i] in b" \t\r\n":
        i += 1
    return w, h, d[i:]


def main():
    a, b = sys.argv[1], sys.argv[2]
    w1, h1, p1 = load(a)
    w2, h2, p2 = load(b)
    if (w1, h1) != (w2, h2):
        print("尺寸不同: %dx%d vs %dx%d" % (w1, h1, w2, h2))
        return
    n = w1 * h1
    diff = sum(1 for k in range(0, n*3, 3)
               if abs(p1[k]-p2[k]) + abs(p1[k+1]-p2[k+1]) + abs(p1[k+2]-p2[k+2]) > 30)
    print("分辨率 %dx%d" % (w1, h1))
    print("变化像素: %d / %d = %.2f%%" % (diff, n, diff*100.0/n))
    print("判定: %s" % ("屏幕有明显变化" if diff*100.0/n > 2 else
                      ("轻微变化" if diff > 0 else "完全相同")))


if __name__ == "__main__":
    main()
