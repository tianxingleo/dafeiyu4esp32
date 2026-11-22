"""USB bridge, DeepSeek voice, and local event input for the StickS3."""

import argparse
import io
import json
import os
import queue
import sqlite3
import time
import wave
from concurrent.futures import ThreadPoolExecutor
from datetime import datetime
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from threading import Thread

import requests
import serial

from memory import Memory


API = "https://api.deepseek.com"
DATA = Path(__file__).resolve().parent.parent / ".whalemate"
EVENT_URL = "http://127.0.0.1:8723/event"
MAX_MIC_BYTES = 16_000 * 2 * 12
MAX_VOICE_BYTES = 24_000 * 2 * 12
MOODS = {"idle", "happy", "annoyed", "sleepy", "dizzy", "surprised", "sad"}
EVENTS = {
    "completed": ("happy", "0", "{project}做完啦！嘿，本鱼就知道你行。"),
    "failed": ("sad", "0", "{project}卡住了。先歇一口气，再来。"),
    "approval": ("surprised", "1", "{project}等你点头啦，快去看一眼。"),
}
AMBIENT = [
    ("idle", "忙你的吧，本鱼在这儿陪你。"),
    ("happy", "要是累了，就跟本鱼一起发会儿呆。"),
    ("annoyed", "哼，本鱼只是来看你有没有记得喝水。"),
]
PERSONA = (
    "你是住在掌上小屏里的蓝色Q版鲸鱼娘大肥鱼，自称本鱼。"
    "用简体中文口语，回复一两句，最多40个字符。平时温柔，偶尔傲娇地喊用户杂鱼。"
    "听到失败或抱怨就安慰，严肃时不嘲讽。不要写动作旁白、Markdown、网址或表情符号。"
)
JSON_REPLY = (
    '只输出一个 JSON 对象，例如 {"reply":"本鱼在呢。","mood":"happy"}。'
    "mood 只能是 idle、happy、annoyed、sleepy、dizzy、surprised、sad 之一。"
)


def clean(value, limit):
    return " ".join(str(value).replace("|", " ").split())[:limit]


def wav_bytes(pcm):
    with io.BytesIO() as output:
        with wave.open(output, "wb") as audio:
            audio.setparams((1, 2, 16_000, 0, "NONE", "not compressed"))
            audio.writeframes(pcm)
        return output.getvalue()


class DeepSeek:
    def __init__(self, key, model, voice):
        self.headers = {"Authorization": f"Bearer {key}"}
        self.model = model
        self.voice = voice

    def post(self, path, *, timeout=(6, 24), **kwargs):
        response = requests.post(
            API + path, headers=self.headers, timeout=timeout, **kwargs
        )
        try:
            response.raise_for_status()
        except requests.HTTPError:
            response.close()
            raise
        return response

    def transcribe(self, pcm):
        with self.post(
            "/v1/tools/asr", files={"file": ("push-to-talk.wav", wav_bytes(pcm), "audio/wav")}
        ) as response:
            try:
                transcript = response.json()["text"]
            except (ValueError, KeyError, TypeError) as error:
                raise ResponseFormatError from error
        if not isinstance(transcript, str):
            raise ResponseFormatError
        return clean(transcript, 800)

    def reply(self, prompt, history, notes):
        messages = []
        if notes:
            messages.append({"role": "user", "content": "以下是我以前让你记住的资料：" + "；".join(notes)})
        messages.extend(history)
        messages.append({"role": "user", "content": prompt})
        try:
            content = self.complete(messages, structured=True)
            data = json.loads(content)
            reply = data["reply"]
            if not isinstance(reply, str) or not reply.strip():
                raise ResponseFormatError
            mood = data.get("mood")
            return mood if isinstance(mood, str) and mood in MOODS else "idle", clean(reply, 42)
        except (ValueError, KeyError, TypeError):
            print("DeepSeek 对话返回空白或异常格式，改用纯文本重试一次。", flush=True)
        return "idle", clean(self.complete(messages, structured=False), 42)

    def complete(self, messages, *, structured):
        instruction = JSON_REPLY if structured else "只输出你要说的中文台词。"
        payload = {
            "model": self.model,
            "messages": [{"role": "system", "content": PERSONA + instruction}, *messages],
            "thinking": {"type": "disabled"},
            "max_tokens": 200,
        }
        if structured:
            payload["response_format"] = {"type": "json_object"}
        with self.post(
            "/chat/completions", json=payload, timeout=(6, 24) if structured else (4, 12)
        ) as response:
            try:
                content = response.json()["choices"][0]["message"]["content"]
            except (ValueError, KeyError, IndexError, TypeError) as error:
                raise ResponseFormatError from error
        if not isinstance(content, str) or not content.strip():
            raise ResponseFormatError
        return content

    def speak(self, line):
        with self.post(
            "/v1/tools/tts", json={"text": line, "voice_id": self.voice, "format": "pcm"},
            stream=True,
        ) as response:
            chunks = bytearray()
            for block in response.iter_content(8192):
                chunks.extend(block)
                if len(chunks) > MAX_VOICE_BYTES:
                    raise ValueError("语音长度超过设备缓冲区")
        if not chunks or len(chunks) % 2:
            raise ValueError("语音采样数据不完整")
        return bytes(chunks)


class ResponseFormatError(ValueError):
    """DeepSeek returned HTTP success without a usable payload."""


class Companion:
    def __init__(self, provider, mock, memory):
        self.provider = provider
        self.mock = mock
        self.memory = memory

    def converse(self, recorded):
        if self.mock:
            return "happy", "听到啦！本鱼正在用模拟语音回应你。", self.chirp()
        if self.provider is None:
            return "sad", "电脑还没配置 DeepSeek 密钥哦。", b""
        stage = "语音转写"
        try:
            prompt = self.provider.transcribe(recorded)
            if not prompt:
                return "surprised", "诶？本鱼没听清，再说一次？", b""
            revision, history, notes = self.memory.snapshot()
            stage = "对话"
            mood, line = self.provider.reply(prompt, history, notes)
            self.memory.remember(prompt, line, revision)
        except ResponseFormatError:
            print(f"DeepSeek {stage}返回了空白或异常结果。", flush=True)
            if stage == "对话":
                return "sad", "本鱼没生成可读回复，再说一次吧。", b""
            return "sad", "转写结果出了点问题，再说一次吧。", b""
        except requests.RequestException as error:
            print(f"DeepSeek {stage}请求失败，{failure_label(error)}", flush=True)
            if failure_label(error) == "HTTP 401":
                return "sad", "电脑上的 DeepSeek 密钥认证失败了。", b""
            if isinstance(error, requests.Timeout):
                return "sad", f"{stage}超时了，等一下再试吧。", b""
            return "sad", f"DeepSeek 的{stage}接口暂时不可用。", b""
        except (OSError, sqlite3.Error) as error:
            print(f"本地记忆读写失败，{type(error).__name__}", flush=True)
            return "sad", "本鱼的记忆本暂时打不开。", b""
        return mood, line, self.speech(line)

    def speech(self, line):
        if self.mock:
            return self.chirp()
        if self.provider is not None:
            try:
                return self.provider.speak(line)
            except (requests.RequestException, ValueError, OSError) as error:
                print(f"语音合成失败，{failure_label(error)}", flush=True)
        return b""

    @staticmethod
    def chirp():
        import math
        import struct

        return b"".join(
            struct.pack("<h", int(1800 * math.sin(i * 0.15) * (1 - i / 6000)))
            for i in range(6000)
        )


def failure_label(error):
    response = getattr(error, "response", None)
    return f"HTTP {response.status_code}" if response is not None else type(error).__name__


class Bridge:
    def __init__(self, companion, events, ambient_after):
        self.companion = companion
        self.events = events
        self.ambient_after = ambient_after
        self.workers = ThreadPoolExecutor(max_workers=1)
        self.last_spoken = -float("inf")
        self.ambient_count = 0

    def run(self, port, once):
        try:
            self.connect(port, once)
        finally:
            self.workers.shutdown(wait=False, cancel_futures=True)

    def connect(self, port, once):
        while True:
            try:
                with serial.Serial(port, 921_600, timeout=0.03, write_timeout=0.7) as device:
                    print(f"串口已打开 {port}，等待大肥鱼回应。", flush=True)
                    self.session(device)
            except (serial.SerialException, OSError) as error:
                if once:
                    raise
                print(f"串口暂不可用 {port}，{type(error).__name__}。三秒后重连。", flush=True)
                time.sleep(3)

    def session(self, device):
        state = "unknown"
        muted = False
        ready = False
        inbound = bytearray()
        capture = bytearray()
        capture_id = ""
        expected = 0
        future = None
        future_id = ""
        voice = b""
        awaiting_ack = None
        acknowledged = 0
        next_ping = 0.0
        pending_event = None
        next_ambient = time.monotonic() + self.ambient_after

        def send(*parts):
            device.write(("|".join(map(str, parts)) + "\n").encode("utf-8"))

        while True:
            now = time.monotonic()
            quiet = datetime.now().hour >= 23 or datetime.now().hour < 7
            if now >= next_ping:
                send("PING", int(quiet))
                next_ping = now + 1
            inbound.extend(device.read(device.in_waiting or 1))
            while b"\n" in inbound:
                raw_line, _, inbound = inbound.partition(b"\n")
                fields = raw_line.rstrip(b"\r").decode("utf-8", errors="replace").split("|")
                kind = fields[0]
                if kind == "STATE" and len(fields) == 4:
                    if not ready:
                        print(f"大肥鱼已在线，固件协议 {fields[3]}。", flush=True)
                    ready, state, muted = fields[3] == "1", fields[1], fields[2] == "1"
                elif kind == "REC" and len(fields) == 3:
                    capture_id = fields[1]
                    capture.clear()
                    expected = int(fields[2]) if fields[2].isdigit() else 0
                    expected = expected if 0 < expected <= MAX_MIC_BYTES else 0
                    state = "busy"
                elif kind == "MIC" and len(fields) == 3 and fields[1] == capture_id and expected:
                    try:
                        capture.extend(bytes.fromhex(fields[2]))
                    except ValueError:
                        expected = 0
                    if len(capture) > expected:
                        expected = 0
                elif kind == "DONE" and len(fields) == 2 and fields[1] == capture_id:
                    if expected and len(capture) == expected:
                        print(f"收到板载麦克风录音，{len(capture) / 32_000:.1f} 秒。", flush=True)
                        if future and future_id == "ambient":
                            future.cancel()
                        future_id = capture_id
                        future = self.workers.submit(self.companion.converse, bytes(capture))
                    else:
                        send("SAY", capture_id, "sad", 0, "录音传输没接好，再试一次吧。")
                    capture_id = ""
                    capture.clear()
                elif kind == "ACK" and len(fields) == 3 and fields[1] == future_id:
                    if awaiting_ack is not None and fields[2] == str(awaiting_ack):
                        acknowledged = awaiting_ack
                        awaiting_ack = None
                elif kind == "SHOWN" and len(fields) == 3:
                    print(f"设备已显示，{fields[1]}。", flush=True)
                    if fields[1] == "event":
                        pending_event = None
                elif kind == "PLAY" and len(fields) == 3:
                    print(f"设备开始播放，{fields[2]} 字节音频。", flush=True)
                elif kind == "PLAYED" and len(fields) == 3:
                    print(f"设备播完，耗时 {fields[2]} 毫秒。", flush=True)
                elif kind == "AUDIOERR" and len(fields) == 3:
                    print(f"设备语音未播放，{fields[2]}。", flush=True)

            if len(inbound) > 4096:
                inbound.clear()
            if future is not None and future.done():
                mood, line, audio = future.result()
                future = None
                if future_id != "ambient" or (state == "idle" and not muted and not quiet):
                    send("SAY", future_id, mood, len(audio), line)
                    if future_id == "ambient" and audio:
                        self.last_spoken = now
                    voice, acknowledged, awaiting_ack = audio, 0, None
                next_ambient = now + self.ambient_after
            if voice and awaiting_ack is None:
                if acknowledged < len(voice):
                    block = voice[acknowledged:acknowledged + 512]
                    send("A", future_id, acknowledged, block.hex())
                    awaiting_ack = acknowledged + len(block)
                    ack_since = now
                else:
                    send("END", future_id)
                    voice = b""
            if awaiting_ack is not None and now - ack_since > 2:
                voice, awaiting_ack = b"", None
            if not ready or state != "idle" or future is not None or voice:
                continue
            if pending_event is None:
                try:
                    pending_event = self.events.get_nowait()
                except queue.Empty:
                    pass
            if pending_event is not None:
                mood, chime, line = pending_event
                send("EV", mood, chime, line)
                # Wait for the device to acknowledge display before retrying.
                state = "event"
                next_ambient = now + self.ambient_after
            elif now >= next_ambient:
                mood, line = AMBIENT[self.ambient_count % len(AMBIENT)]
                self.ambient_count += 1
                next_ambient = now + self.ambient_after
                if not quiet and not muted and self.ambient_count % 3 == 0 and now - self.last_spoken >= 3600:
                    future_id = "ambient"
                    future = self.workers.submit(lambda: (mood, line, self.companion.speech(line)))
                elif not muted:
                    send("EV", mood, 0, line)


def event_server(events):
    class Handler(BaseHTTPRequestHandler):
        def do_POST(self):
            if self.path != "/event":
                self.send_error(404)
                return
            if self.headers.get_content_type() != "application/json":
                self.send_error(415)
                return
            try:
                length = int(self.headers.get("Content-Length", 0))
                if not 0 < length <= 1024:
                    raise ValueError
                payload = json.loads(self.rfile.read(length))
                mood, chime, template = EVENTS[payload["kind"]]
                project = clean(payload.get("project", "这个项目"), 12) or "这个项目"
                events.put_nowait((mood, chime, template.format(project=project)))
            except (ValueError, KeyError, TypeError, AttributeError, UnicodeError):
                self.send_error(400, "Invalid event")
                return
            except queue.Full:
                self.send_error(429, "Event queue full")
                return
            self.send_response(202)
            self.end_headers()

        def log_message(self, *_args):
            pass

    return ThreadingHTTPServer(("127.0.0.1", 8723), Handler)


def main():
    parser = argparse.ArgumentParser(description="大肥鱼电脑端服务")
    commands = parser.add_subparsers(dest="command", required=True)
    serve = commands.add_parser("serve", help="连接 StickS3")
    serve.add_argument("--port", required=True)
    serve.add_argument("--mock-voice", action="store_true")
    serve.add_argument("--once", action="store_true")
    serve.add_argument("--ambient-minutes", type=float, default=20)
    event = commands.add_parser("event", help="发送模拟编程事件")
    event.add_argument("kind", choices=EVENTS)
    event.add_argument("--project", default="StickS3")
    memory_cmd = commands.add_parser("memory", help="查看或清除本地记忆")
    memory_cmd.add_argument("action", choices=("list", "clear"))
    args = parser.parse_args()

    if args.command == "event":
        with requests.post(
            EVENT_URL, json={"kind": args.kind, "project": args.project}, timeout=3
        ) as response:
            response.raise_for_status()
        print("事件已进入大肥鱼队列。")
        return
    memory = Memory(DATA / "memory.sqlite3")
    if args.command == "memory":
        if args.action == "clear":
            memory.clear()
            print("本地记忆已清除。")
        else:
            print(json.dumps(memory.notes(), ensure_ascii=False, indent=2))
        return

    if args.ambient_minutes <= 0:
        parser.error("--ambient-minutes 必须大于 0")
    key = os.getenv("DEEPSEEK_API_KEY")
    provider = DeepSeek(key, os.getenv("WHALE_MODEL", "deepseek-flash"), "stella") if key else None
    events = queue.Queue(maxsize=8)
    companion = Companion(provider, args.mock_voice, memory)
    with event_server(events) as server:
        Thread(target=server.serve_forever, daemon=True).start()
        try:
            Bridge(companion, events, args.ambient_minutes * 60).run(args.port, args.once)
        except KeyboardInterrupt:
            print("大肥鱼电脑端服务已停止。")
        finally:
            server.shutdown()


if __name__ == "__main__":
    main()
