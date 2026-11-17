#!/usr/bin/env python3
"""Agent Home 推送端:统计本机 Claude Code / Codex 的 Token 用量,经 USB 串口推给 ESP32 屏。

用法:
  python3 push_usage.py              # 找到 /dev/cu.usbmodem* 后每 60 秒推一次
  python3 push_usage.py --once       # 只推一次
  python3 push_usage.py --dry-run    # 只打印要推的行,不碰串口

第 1 页口径:Token = 输入 + 输出 + 缓存写 + 缓存读(和 Claude Code 日志里 usage 四项之和一致);
日期按本机时区切分。日志只增量读取新追加的字节,首次启动会把最近 8 天全扫一遍。

第 2 页额度:Claude / Gemini 读 AgentHome.app 落盘的 ~/AgentHome/signals/*-usage.json,
Codex 读会话日志里最新的 rate_limits。Claude / Codex 是 Pro 周额度,只推 7 天窗口;
Gemini 推 5 小时(外圈)和 7 天(内圈)两个窗口。
每个窗口另推紧张程度(按目前速度推算到重置时会用掉多少)和时间刻度(离重置还剩的时间比例)。
"""
import argparse
import datetime as dt
import glob
import json
import os
import select
import sys
import termios
import time

DAYS = 7
CLAUDE_GLOB = os.path.expanduser("~/.claude/projects/**/*.jsonl")
CODEX_GLOB = os.path.expanduser("~/.codex/sessions/*/*/*/*.jsonl")
SIGNALS = os.path.expanduser("~/AgentHome/signals")
STALE_SIGNAL_MS = 2 * 3600 * 1000


def local_day(ts: str) -> dt.date:
    return dt.datetime.fromisoformat(ts.replace("Z", "+00:00")).astimezone().date()


class Tally:
    """按天累计 Token;按文件记住读到的位置,下次只读新增内容。"""

    def __init__(self) -> None:
        self.offsets: dict[str, int] = {}
        self.claude: dict[dt.date, int] = {}
        self.codex: dict[dt.date, int] = {}
        self.output: dict[dt.date, int] = {}
        self.seen: set[tuple[str, str]] = set()
        self.codex_total: dict[str, int] = {}
        self.codex_limits: tuple[str, dict] | None = None  # (时间戳, rate_limits)

    def _new_lines(self, path: str):
        size = os.path.getsize(path)
        start = self.offsets.get(path, 0)
        if size < start:  # 文件被截断或重写,从头读
            start = 0
        if size == start:
            return
        with open(path, "rb") as f:
            f.seek(start)
            data = f.read(size - start)
        # 最后一行可能还没写完,留到下次
        cut = data.rfind(b"\n") + 1
        self.offsets[path] = start + cut
        for raw in data[:cut].splitlines():
            yield raw

    def scan(self, since: dt.date) -> None:
        cutoff = time.time() - (DAYS + 1) * 86400
        for path in glob.glob(CLAUDE_GLOB, recursive=True):
            if os.path.getmtime(path) < cutoff:
                continue
            for raw in self._new_lines(path):
                if b'"usage"' not in raw:
                    continue
                self._claude_line(raw, since)
        for path in glob.glob(CODEX_GLOB):
            if os.path.getmtime(path) < cutoff:
                continue
            for raw in self._new_lines(path):
                if b'"token_count"' not in raw:
                    continue
                self._codex_line(path, raw, since)

    def _claude_line(self, raw: bytes, since: dt.date) -> None:
        try:
            d = json.loads(raw)
        except ValueError:
            return
        msg = d.get("message")
        if not isinstance(msg, dict) or not isinstance(msg.get("usage"), dict):
            return
        # 同一条回复会因流式写入出现多行,按 message.id + requestId 去重
        key = (str(msg.get("id")), str(d.get("requestId")))
        if key in self.seen:
            return
        self.seen.add(key)
        day = local_day(d.get("timestamp", "1970-01-01T00:00:00Z"))
        if day < since:
            return
        u = msg["usage"]
        total = sum(int(u.get(k) or 0) for k in (
            "input_tokens", "output_tokens", "cache_creation_input_tokens", "cache_read_input_tokens"))
        self.claude[day] = self.claude.get(day, 0) + total
        self.output[day] = self.output.get(day, 0) + int(u.get("output_tokens") or 0)

    def _codex_line(self, path: str, raw: bytes, since: dt.date) -> None:
        try:
            d = json.loads(raw)
        except ValueError:
            return
        payload = d.get("payload") or {}
        limits = payload.get("rate_limits")
        ts = d.get("timestamp", "")
        if isinstance(limits, dict) and (self.codex_limits is None or ts >= self.codex_limits[0]):
            self.codex_limits = (ts, limits)
        info = payload.get("info") or {}
        usage = info.get("total_token_usage")
        if not isinstance(usage, dict):
            return
        # Codex 会重复发同样的累计值,用会话累计值的增量计数,避免重复
        total = int(usage.get("total_tokens") or 0)
        out = int(usage.get("output_tokens") or 0)
        prev_total = self.codex_total.get(path, 0)
        prev_out = self.codex_total.get(path + "#out", 0)
        if total <= prev_total:
            return
        self.codex_total[path] = total
        self.codex_total[path + "#out"] = out
        day = local_day(d.get("timestamp", "1970-01-01T00:00:00Z"))
        if day < since:
            return
        self.codex[day] = self.codex.get(day, 0) + total - prev_total
        self.output[day] = self.output.get(day, 0) + max(out - prev_out, 0)


def build_line(t: Tally, today: dt.date, now: dt.datetime) -> str:
    days = [today - dt.timedelta(days=DAYS - 1 - i) for i in range(DAYS)]
    per_day = [t.claude.get(d, 0) + t.codex.get(d, 0) for d in days]
    first_dow = (days[0].weekday() + 1) % 7  # Python 周一=0,屏上周日=0
    fields = {
        "hm": now.strftime("%H:%M"),
        "today": per_day[-1],
        "prev": per_day[-2],
        "avg": sum(per_day) // DAYS,
        "cc": t.claude.get(today, 0),
        "cx": t.codex.get(today, 0),
        "out": t.output.get(today, 0),
        "days": ",".join(str(round(v / 1e6)) for v in per_day),
        "dow": first_dow,
    }
    return "AH1 " + " ".join(f"{k}={v}" for k, v in fields.items())


def fmt_tokens(n: float) -> str:
    for div, unit in ((1e9, "B"), (1e6, "M"), (1e3, "K")):
        if n >= div:
            return f"{n / div:.1f}{unit}"
    return str(int(n))


def fmt_reset(reset_ms: int, now: dt.datetime) -> str:
    at = dt.datetime.fromtimestamp(reset_ms / 1000)
    left = at - now
    if left.total_seconds() <= 0:
        return "已重置"
    if left < dt.timedelta(hours=24):
        mins = int(left.total_seconds() // 60)
        return f"{mins // 60}:{mins % 60:02d} 后重置"
    return f"{at:%m-%d} 重置"


def read_signal(name: str) -> dict | None:
    try:
        with open(os.path.join(SIGNALS, name)) as f:
            return json.load(f)
    except (OSError, ValueError):
        return None


def remain(win: dict | None) -> int | None:
    return None if win is None else max(0, min(100, round(100 - win["used"])))


def pace(win: dict | None, now_ms: float) -> tuple[int, int]:
    """(紧张程度, 时间刻度)。紧张程度 0 不判断、1 够用、2 偏快、3 撑不到重置;
    按目前速度推算到重置时用掉多少:<90% 够用,90%–110% 偏快,>110% 撑不到。
    时间刻度是离重置还剩的时间占整个窗口的百分比,-1 表示没有。"""
    if win is None or not win["min"]:
        return 0, -1
    left = max(0.0, min(1.0, (win["reset"] - now_ms) / (win["min"] * 60000)))
    elapsed = 1 - left
    if elapsed < 0.1:  # 窗口刚开始,样本太少推不准
        level = 0
    else:
        projected = win["used"] / elapsed
        level = 1 if projected < 90 else 2 if projected <= 110 else 3
    return level, round(left * 100)


def build_quota_lines(t: Tally, now: dt.datetime) -> list[str]:
    now_ms = now.timestamp() * 1000

    def windows(sig: dict | None) -> dict[str, dict]:
        return {w.get("id", ""): {"used": w["used_percent"], "reset": w["resets_at_ms"],
                                     "min": w.get("window_minutes", 0)}
                for w in (sig or {}).get("windows", [])}

    def stale(sig: dict | None) -> bool:
        return sig is None or now_ms - sig.get("fetched_at_ms", 0) > STALE_SIGNAL_MS

    sig = read_signal("claude-usage.json")
    claude_week = windows(sig).get("seven_day")
    claude_stale = stale(sig)

    codex_week = None
    if t.codex_limits:
        for key in ("primary", "secondary"):
            w = t.codex_limits[1].get(key)
            if isinstance(w, dict) and w.get("window_minutes") == 10080:
                codex_week = {"used": w.get("used_percent", 0), "reset": int(w.get("resets_at", 0)) * 1000,
                              "min": 10080}

    sig = read_signal("agy-usage.json")
    gem = windows(sig)
    gem_5h, gem_week = gem.get("gemini-5h"), gem.get("gemini-weekly")
    gem_stale = stale(sig)

    lines = []
    for i, (name, color, win) in enumerate([("Claude", "d97757", claude_week), ("Codex", "4a9a6b", codex_week)]):
        if win is None:
            lines.append(f"AHM {i}|0|{name}|暂无周额度数据||{color}")
        else:
            level, tick = pace(win, now_ms)
            lines.append(f"AHM {i}|{remain(win)}|{name}|周额度 · {fmt_reset(win['reset'], now)}||{color}||{level}|{tick}")
    if gem_5h is None and gem_week is None:
        lines.append("AHM 2|0|Gemini|暂无额度数据||6a8caf")
    else:
        rows = [f"{label} {remain(w)}% · {fmt_reset(w['reset'], now)}" if w else f"{label} --"
                for label, w in (("5h", gem_5h), ("周", gem_week))]
        inner = remain(gem_week)
        (level, tick), (inner_level, inner_tick) = pace(gem_5h, now_ms), pace(gem_week, now_ms)
        lines.append(f"AHM 2|{remain(gem_5h) or 0}|Gemini|{rows[0]}|{rows[1]}|6a8caf|{'' if inner is None else inner}"
                     f"|{level}|{tick}|{inner_level}|{inner_tick}")

    stale_names = [n for n, st in (("Claude", claude_stale), ("Codex", codex_week is None), ("Gemini", gem_stale)) if st]
    # 数据正常时底部显示紧张程度图例(固件画),只在数据过期时推提示
    warn = "、".join(stale_names) + " 额度数据超过 2 小时没更新" if stale_names else ""
    lines.append("AHW " + warn)
    return lines


def read_until(fd: int, token: bytes, count: int, timeout: float) -> bytes:
    buf = b""
    deadline = time.time() + timeout
    while buf.count(token) < count and time.time() < deadline:
        ready, _, _ = select.select([fd], [], [], 0.1)
        if ready:
            try:
                buf += os.read(fd, 4096)
            except BlockingIOError:
                pass
    return buf


def push_lines(fd: int, lines: list[str]) -> int:
    """先握手再逐行写入,返回板子回 ok 的行数。

    串口刚打开时板子那头还没就绪,最先写进去的几行会丢,所以先反复发 AH? 直到板子回 diag。
    """
    for _ in range(10):
        os.write(fd, b"AH?\n")
        if b"diag" in read_until(fd, b"diag", 1, 0.3):
            break
    else:
        return 0
    read_until(fd, b"ok", 1, 0.2)  # 吃掉 AH? 自己的那个 ok
    for line in lines:
        os.write(fd, (line + "\n").encode())
        termios.tcdrain(fd)
        time.sleep(0.05)  # 板子重画屏幕时来不及收,行与行之间留点空
    return read_until(fd, b"ok", len(lines), 2).count(b"ok")


def open_port(path: str) -> int:
    fd = os.open(path, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    attrs = termios.tcgetattr(fd)
    attrs[0] = 0  # iflag
    attrs[1] = 0  # oflag:不把 \n 换成 \r\n
    attrs[3] = 0  # lflag:关回显和行缓冲
    termios.tcsetattr(fd, termios.TCSANOW, attrs)
    return fd


def find_port() -> str | None:
    ports = sorted(glob.glob("/dev/cu.usbmodem*"))
    return ports[0] if ports else None


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--once", action="store_true")
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--port")
    ap.add_argument("--interval", type=int, default=60)
    args = ap.parse_args()

    tally = Tally()
    while True:
        now = dt.datetime.now()
        today = now.date()
        tally.scan(today - dt.timedelta(days=DAYS - 1))
        lines = [build_line(tally, today, now)] + build_quota_lines(tally, now)
        print("\n".join(lines), flush=True)
        if not args.dry_run:
            # 每次推完就关掉串口,不长期占着端口,烧录固件时不用先停脚本
            try:
                port = args.port or find_port()
                if port is None:
                    raise OSError("没找到 /dev/cu.usbmodem*,屏是否插着?")
                fd = open_port(port)
                try:
                    acked = push_lines(fd, lines)
                finally:
                    os.close(fd)
                print(f"板子确认 {acked}/{len(lines)} 行", flush=True)
                if acked < len(lines):
                    print(f"推送不完整:板子只确认了 {acked}/{len(lines)} 行", file=sys.stderr, flush=True)
            except OSError as e:
                print(f"推送失败:{e}", file=sys.stderr, flush=True)
                if args.once:
                    return 1
        if args.once:
            return 0
        time.sleep(args.interval)


if __name__ == "__main__":
    sys.exit(main())
