#!/usr/bin/env python3
"""Локальная проверка реакции camera_demo на сбои RTSP без внешних сервисов."""

import argparse
import base64
import json
import signal
import socket
import struct
import subprocess
import threading
import time
from pathlib import Path
from typing import Optional


WIDTH = 640
HEIGHT = 360
FPS = 10


def run(command: list[str], timeout: float = 30.0) -> subprocess.CompletedProcess:
    result = subprocess.run(command, capture_output=True, text=True, timeout=timeout, check=False)
    if result.returncode != 0:
        raise RuntimeError(f"Команда завершилась с кодом {result.returncode}: {command}\n{result.stderr}")
    return result


def annexb_nals(data: bytes) -> list[bytes]:
    """Разбирает Annex-B NAL, принимая оба стандартных варианта start code."""
    starts: list[tuple[int, int]] = []
    index = 0
    while index < len(data) - 3:
        if data[index:index + 4] == b"\0\0\0\1":
            starts.append((index, 4))
            index += 4
        elif data[index:index + 3] == b"\0\0\1":
            starts.append((index, 3))
            index += 3
        else:
            index += 1
    nals = [data[pos + size:(starts[i + 1][0] if i + 1 < len(starts) else len(data))]
            for i, (pos, size) in enumerate(starts)]
    return [nal.rstrip(b"\0") for nal in nals if nal]


def access_units(nals: list[bytes]) -> list[list[bytes]]:
    units: list[list[bytes]] = []
    for nal in nals:
        if nal[0] & 0x1F == 9:
            units.append([])
        if units:
            units[-1].append(nal)
    return [unit for unit in units if unit]


def make_rtp_packets(nal: bytes, sequence: int, timestamp: int, marker: bool,
                     ssrc: int) -> tuple[list[bytes], int]:
    header = lambda seq, mark: struct.pack("!BBHII", 0x80, 96 | (0x80 if mark else 0), seq,
                                           timestamp, ssrc)
    if len(nal) <= 1400:
        return [header(sequence, marker) + nal], (sequence + 1) & 0xFFFF
    indicator = (nal[0] & 0xE0) | 28
    nal_type = nal[0] & 0x1F
    fragments = [nal[1 + offset:1 + offset + 1398] for offset in range(0, len(nal) - 1, 1398)]
    packets = []
    for i, fragment in enumerate(fragments):
        fu_header = nal_type | (0x80 if i == 0 else 0) | (0x40 if i == len(fragments) - 1 else 0)
        packets.append(header(sequence, marker and i == len(fragments) - 1) + bytes((indicator, fu_header)) + fragment)
        sequence = (sequence + 1) & 0xFFFF
    return packets, sequence


class RtspFixture:
    """RTSP fixture обслуживает последовательные TCP подключения camera_demo."""

    def __init__(self, units: list[list[bytes]], reconnect: bool = False,
                 defer_listen: bool = False) -> None:
        self.units = units
        self.reconnect = reconnect
        self.defer_listen = defer_listen
        self.listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        try:
            self.listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            self.listener.bind(("127.0.0.1", 0))
            self.listener.settimeout(1.0)
            self.port = self.listener.getsockname()[1]
        except OSError:
            self.listener.close()
            raise
        self.cleanup = threading.Event()
        self.listen_ready = threading.Event()
        self.stop_stream = threading.Event()
        self.disconnect = threading.Event()
        self.connected = threading.Event()
        self.playing = threading.Event()
        self.play_count = 0
        self.play_condition = threading.Condition()
        self.stalled = threading.Event()
        self.error: Optional[BaseException] = None
        self.thread = threading.Thread(target=self._serve, name="rtsp-fixture", daemon=True)

    def __enter__(self) -> "RtspFixture":
        if not self.defer_listen:
            self.start_listening()
        self.thread.start()
        return self

    def __exit__(self, exc_type: object, exc: object, traceback: object) -> None:
        self.cleanup.set()
        self.listen_ready.set()
        self.stop_stream.set()
        self.disconnect.set()
        self.listener.close()
        self.thread.join(timeout=3.0)
        if self.thread.is_alive():
            raise RuntimeError("RTSP fixture thread не завершился")
        if exc is None and self.error is not None:
            raise RuntimeError(f"Ошибка RTSP fixture: {self.error}") from self.error

    def stall_stream(self) -> None:
        self.stop_stream.set()

    def fail_connection(self) -> None:
        self.disconnect.set()

    def start_listening(self) -> None:
        self.listener.listen(1)
        self.listen_ready.set()

    def wait_for_playing(self, count: int, timeout: float) -> bool:
        deadline = time.monotonic() + timeout
        with self.play_condition:
            while self.play_count < count:
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    return False
                self.play_condition.wait(remaining)
            return True

    def _serve(self) -> None:
        client: Optional[socket.socket] = None
        try:
            self.listen_ready.wait()
            session_index = 0
            while not self.cleanup.is_set():
                try:
                    client, _ = self.listener.accept()
                except socket.timeout:
                    continue
                except OSError:
                    if self.cleanup.is_set():
                        return
                    raise
                if session_index and not self.reconnect:
                    client.close()
                    return
                session_index += 1
                self.disconnect.clear()
                self.connected.set()
                client.settimeout(1.0)
                buffer = b""
                session = f"1234567{session_index}"
                sps = next(nal for unit in self.units for nal in unit if nal[0] & 0x1F == 7)
                pps = next(nal for unit in self.units for nal in unit if nal[0] & 0x1F == 8)
                reconnect_client = False
                while not self.cleanup.is_set() and not reconnect_client:
                    try:
                        chunk = client.recv(4096)
                    except socket.timeout:
                        continue
                    if not chunk:
                        reconnect_client = True
                        continue
                    buffer += chunk
                    while b"\r\n\r\n" in buffer:
                        header, buffer = buffer.split(b"\r\n\r\n", 1)
                        lines = header.decode("ascii", "replace").split("\r\n")
                        method, uri, _ = lines[0].split(" ", 2)
                        headers = {line.split(":", 1)[0].lower(): line.split(":", 1)[1].strip()
                                   for line in lines[1:] if ":" in line}
                        cseq = headers.get("cseq", "1")
                        if method == "OPTIONS":
                            self._reply(client, cseq, "Public: OPTIONS, DESCRIBE, SETUP, PLAY, GET_PARAMETER, TEARDOWN\r\n")
                        elif method == "DESCRIBE":
                            sdp = ("v=0\r\n"
                                   "o=- 0 0 IN IP4 127.0.0.1\r\n"
                                   "s=Camera fault fixture\r\n"
                                   "c=IN IP4 127.0.0.1\r\n"
                                   "t=0 0\r\n"
                                   "m=video 0 RTP/AVP 96\r\n"
                                   "a=rtpmap:96 H264/90000\r\n"
                                   "a=framerate:10\r\n"
                                   f"a=fmtp:96 packetization-mode=1; sprop-parameter-sets={base64.b64encode(sps).decode()},{base64.b64encode(pps).decode()}\r\n"
                                   "a=control:trackID=0\r\n")
                            body = sdp.encode("ascii")
                            self._reply(client, cseq, "Content-Type: application/sdp\r\n", body)
                        elif method == "SETUP":
                            transport = headers.get("transport", "")
                            if "RTP/AVP/TCP" not in transport:
                                raise RuntimeError(f"Ожидался RTP/AVP/TCP: {transport}")
                            self._reply(client, cseq, f"Session: {session}\r\nTransport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n")
                        elif method in ("PLAY", "GET_PARAMETER", "TEARDOWN"):
                            self._reply(client, cseq, f"Session: {session}\r\n")
                            if method == "PLAY":
                                self.playing.set()
                                with self.play_condition:
                                    self.play_count += 1
                                    self.play_condition.notify_all()
                                self._stream(client)
                                reconnect_client = True
                                break
                        else:
                            self._reply(client, cseq, f"Session: {session}\r\n")
                client.close()
                client = None
                if not self.reconnect or self.cleanup.is_set():
                    return
        except (ConnectionResetError, BrokenPipeError):
            # camera_demo может сбросить TCP-сессию при SIGINT во время отправки RTP.
            pass
        except OSError as error:
            if not self.cleanup.is_set() and not self.disconnect.is_set():
                self.error = error
        except BaseException as error:
            self.error = error
        finally:
            if client is not None:
                try:
                    client.close()
                except OSError:
                    pass

    @staticmethod
    def _reply(client: socket.socket, cseq: str, headers: str = "", body: bytes = b"") -> None:
        response = (f"RTSP/1.0 200 OK\r\nCSeq: {cseq}\r\n{headers}"
                    f"Content-Length: {len(body)}\r\n\r\n").encode("ascii") + body
        client.sendall(response)

    def _stream(self, client: socket.socket) -> None:
        sequence = 1
        timestamp = 0
        ssrc = 0x12345678
        start = time.monotonic()
        unit_index = 0
        while not self.cleanup.is_set() and not self.disconnect.is_set() and not self.stop_stream.is_set():
            target = start + unit_index / FPS
            while time.monotonic() < target:
                if self.cleanup.is_set() or self.disconnect.is_set() or self.stop_stream.is_set():
                    break
                time.sleep(min(0.02, target - time.monotonic()))
            if self.cleanup.is_set() or self.disconnect.is_set() or self.stop_stream.is_set():
                break
            unit = self.units[unit_index % len(self.units)]
            for nal_index, nal in enumerate(unit):
                packets, sequence = make_rtp_packets(nal, sequence, timestamp,
                                                     nal_index == len(unit) - 1, ssrc)
                for packet in packets:
                    framed = b"$\x00" + struct.pack("!H", len(packet)) + packet
                    client.sendall(framed)
            timestamp = (timestamp + 9000) & 0xFFFFFFFF
            unit_index += 1
        if self.stop_stream.is_set() and not self.disconnect.is_set():
            self.stalled.set()
            while not self.cleanup.is_set() and not self.disconnect.is_set():
                time.sleep(0.05)


def prepare_stream(ffmpeg: str, work_dir: Path) -> list[list[bytes]]:
    source = work_dir / "fixture.h264"
    run([ffmpeg, "-hide_banner", "-loglevel", "error", "-y", "-f", "lavfi", "-i",
         f"testsrc2=size={WIDTH}x{HEIGHT}:rate={FPS}", "-frames:v", "20", "-an",
         "-c:v", "libx264", "-bf", "0", "-g", "10", "-x264-params",
         "aud=1:repeat-headers=1", "-f", "h264", str(source)])
    units = access_units(annexb_nals(source.read_bytes()))
    if len(units) < 10 or not any(nal[0] & 0x1F == 7 for unit in units for nal in unit) or not any(
            nal[0] & 0x1F == 8 for unit in units for nal in unit):
        raise AssertionError(f"FFmpeg не создал ожидаемые H.264 access units: {len(units)}")
    return units


def wait_for_output(path: Path, timeout: float = 15.0, minimum_size: int = 32768) -> None:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if path.exists() and path.stat().st_size >= minimum_size:
            return
        time.sleep(0.05)
    size = path.stat().st_size if path.exists() else 0
    raise AssertionError(f"camera_demo не записал {minimum_size} байт за {timeout:.0f} с; размер {size}: {path}")


def probe_video(ffprobe: str, ffmpeg: str, path: Path) -> int:
    parsed = json.loads(run([ffprobe, "-v", "error", "-count_frames", "-select_streams", "v:0",
                             "-show_entries", "stream=codec_name,width,height,nb_read_frames",
                             "-of", "json", str(path)]).stdout)
    streams = parsed.get("streams", [])
    if len(streams) != 1:
        raise AssertionError(f"В MP4 ожидался один видеопоток: {path}")
    stream = streams[0]
    if stream.get("codec_name") != "h264" or (stream.get("width"), stream.get("height")) != (WIDTH, HEIGHT):
        raise AssertionError(f"Неожиданный видеопоток: {stream}")
    frames = int(stream.get("nb_read_frames", "0"))
    if frames < 1:
        raise AssertionError(f"В MP4 нет декодируемых кадров: {path}")
    run([ffmpeg, "-hide_banner", "-loglevel", "error", "-xerror", "-i", str(path), "-f", "null", "-"], 15)
    return frames


def execute_scenario(demo: str, ffmpeg: str, ffprobe: str, work_dir: Path,
                     scenario: str) -> dict[str, object]:
    output = work_dir / f"{scenario}.mp4"
    log = work_dir / f"{scenario}.log"
    units = prepare_stream(ffmpeg, work_dir)
    output.write_bytes(b"")
    with RtspFixture(units) as fixture:
        with log.open("w", encoding="utf-8") as log_file:
            process = subprocess.Popen([demo, "--transcode", f"rtsp://127.0.0.1:{fixture.port}/test", str(output)],
                                       stdout=log_file, stderr=subprocess.STDOUT, text=True)
            try:
                if not fixture.playing.wait(15):
                    raise AssertionError("camera_demo не выполнил RTSP PLAY за 15 с")
                wait_for_output(output)
                time.sleep(1.0)
                if scenario == "stop":
                    fixture.stall_stream()
                    if not fixture.stalled.wait(2.0):
                        raise AssertionError("RTSP fixture не остановила передачу перед SIGINT")
                    time.sleep(0.3)
                    fault_started = time.monotonic()
                    process.send_signal(signal.SIGINT)
                elif scenario == "disconnect":
                    fault_started = time.monotonic()
                    fixture.fail_connection()
                else:
                    fault_started = time.monotonic()
                    fixture.stall_stream()
                try:
                    return_code = process.wait(timeout=13 if scenario == "stall" else 3)
                except subprocess.TimeoutExpired as error:
                    raise AssertionError(f"camera_demo завис в сценарии {scenario}") from error
                fault_elapsed = time.monotonic() - fault_started
            finally:
                if process.poll() is None:
                    process.kill()
                    process.wait(timeout=3)
    frames = probe_video(ffprobe, ffmpeg, output)
    details = log.read_text(encoding="utf-8")
    if "Обработано:" not in details:
        raise AssertionError(f"Не найдено сообщение о завершении записи:\n{details}")
    if scenario in ("stall", "disconnect"):
        if return_code == 0 or "[main] Ошибка чтения:" not in details:
            raise AssertionError(f"{scenario}: ожидалась ошибка чтения и ненулевой код:\n{details}")
    if scenario == "stop" and (return_code != 0 or "Пользовательское прерывание (Ctrl+C)" not in details):
        raise AssertionError(f"Штатное прерывание должно завершиться с кодом 0 и сообщением SIGINT, получен {return_code}:\n{details}")
    limit = 13 if scenario == "stall" else 3
    if fault_elapsed > limit:
        raise AssertionError(f"{scenario}: завершение после сбоя заняло {fault_elapsed:.2f} с, лимит {limit} с")
    return {"scenario": scenario, "return_code": return_code, "frames": frames,
            "fault_elapsed_seconds": round(fault_elapsed, 3), "log": str(log), "output": str(output)}


def execute_reconnect_scenario(demo: str, ffmpeg: str, ffprobe: str, work_dir: Path,
                               scenario: str) -> dict[str, object]:
    output = work_dir / f"{scenario}_{time.monotonic_ns()}.mp4"
    second_output = output.with_name(f"{output.stem}_session2{output.suffix}")
    log = output.with_suffix(".log")
    units = prepare_stream(ffmpeg, work_dir)
    reconnect_budget = 1 if scenario == "reconnect_exhausted" else 2
    with RtspFixture(units, reconnect=True, defer_listen=scenario == "initial_unavailable") as fixture:
        with log.open("w", encoding="utf-8") as log_file:
            process = subprocess.Popen([demo, "--reconnect", str(reconnect_budget), "--transcode",
                                        f"rtsp://127.0.0.1:{fixture.port}/test", str(output)],
                                       stdout=log_file, stderr=subprocess.STDOUT, text=True)
            try:
                if scenario == "initial_unavailable":
                    deadline = time.monotonic() + 5
                    while time.monotonic() < deadline:
                        log_file.flush()
                        if "Переподключение: попытка 1" in log.read_text(encoding="utf-8"):
                            break
                        if process.poll() is not None:
                            raise AssertionError("camera_demo завершился до retry ошибки инициализации")
                        time.sleep(0.02)
                    else:
                        raise AssertionError("Не найден retry после ошибки первоначального RTSP connect")
                    fixture.start_listening()
                if not fixture.wait_for_playing(1, 15):
                    raise AssertionError("camera_demo не выполнил первый RTSP PLAY за 15 с")
                active_output = second_output if scenario == "initial_unavailable" else output
                wait_for_output(active_output)
                time.sleep(0.3)
                if scenario == "initial_unavailable":
                    process.send_signal(signal.SIGINT)
                    return_code = process.wait(timeout=4)
                    if return_code != 0:
                        raise AssertionError(f"SIGINT после начального retry завершился с кодом {return_code}")
                elif scenario == "reconnect_stop":
                    fixture.fail_connection()
                    deadline = time.monotonic() + 4
                    while time.monotonic() < deadline:
                        log_file.flush()
                        if "Переподключение: попытка 1" in log.read_text(encoding="utf-8"):
                            break
                        if process.poll() is not None:
                            raise AssertionError("camera_demo завершился до начала backoff")
                        time.sleep(0.02)
                    else:
                        raise AssertionError("Не найдено сообщение о переподключении перед SIGINT")
                    process.send_signal(signal.SIGINT)
                    return_code = process.wait(timeout=4)
                    if return_code != 0:
                        raise AssertionError(f"SIGINT во время backoff завершился с кодом {return_code}")
                else:
                    fixture.fail_connection()
                    if not fixture.wait_for_playing(2, 10):
                        raise AssertionError("camera_demo не подключился повторно за 10 с")
                    wait_for_output(second_output, timeout=10)
                    time.sleep(0.3)
                    if scenario == "reconnect_exhausted":
                        fixture.fail_connection()
                        return_code = process.wait(timeout=8)
                        if return_code != 1:
                            raise AssertionError(f"После исчерпания бюджета ожидался код 1, получен {return_code}")
                    else:
                        process.send_signal(signal.SIGINT)
                        return_code = process.wait(timeout=4)
                        if return_code != 0:
                            raise AssertionError(f"SIGINT после переподключения завершился с кодом {return_code}")
            finally:
                if process.poll() is None:
                    process.kill()
                    process.wait(timeout=3)
    details = log.read_text(encoding="utf-8")
    if "Переподключение: попытка 1" not in details:
        raise AssertionError(f"Не найдено сообщение о переподключении:\n{details}")
    first_output = second_output if scenario == "initial_unavailable" else output
    first_frames = probe_video(ffprobe, ffmpeg, first_output)
    frames = [first_frames]
    if scenario not in ("reconnect_stop", "initial_unavailable"):
        frames.append(probe_video(ffprobe, ffmpeg, second_output))
    if scenario == "reconnect_exhausted":
        if "Ошибка чтения" not in details or "Попытки переподключения исчерпаны" not in details:
            raise AssertionError(f"После исчерпания бюджета ожидалась ошибка чтения:\n{details}")
        if fixture.play_count != 2:
            raise AssertionError(f"До исчерпания бюджета ожидалось ровно два RTSP PLAY, получено {fixture.play_count}")
    elif scenario in ("reconnect_stop", "initial_unavailable") and "Пользовательское прерывание (Ctrl+C)" not in details:
        raise AssertionError(f"Не найдено сообщение SIGINT во время backoff:\n{details}")
    elif scenario == "reconnect" and "Пользовательское прерывание (Ctrl+C)" not in details:
        raise AssertionError(f"Не найдено сообщение SIGINT после переподключения:\n{details}")
    return {"scenario": scenario, "return_code": return_code, "frames": frames,
            "log": str(log), "outputs": [str(output), str(second_output)] if scenario != "initial_unavailable"
            and len(frames) == 2 else [str(first_output)]}


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--demo", required=True)
    parser.add_argument("--ffmpeg", required=True)
    parser.add_argument("--ffprobe", required=True)
    parser.add_argument("--work-dir")
    parser.add_argument("--scenario", choices=("stall", "disconnect", "stop", "reconnect",
                                                 "reconnect_exhausted", "reconnect_stop",
                                                 "initial_unavailable"), action="append")
    args = parser.parse_args()
    demo = str(Path(args.demo).resolve())
    ffmpeg = str(Path(args.ffmpeg).resolve())
    ffprobe = str(Path(args.ffprobe).resolve())
    work_dir = Path(args.work_dir).resolve() if args.work_dir else Path(__file__).resolve().parent.parent / "tmp" / "rtsp_faults"
    work_dir.mkdir(parents=True, exist_ok=True)
    scenarios = args.scenario or ["stall", "disconnect", "stop", "reconnect", "reconnect_exhausted",
                                  "reconnect_stop", "initial_unavailable"]
    results = []
    for scenario in scenarios:
        if scenario in ("reconnect", "reconnect_exhausted", "reconnect_stop", "initial_unavailable"):
            results.append(execute_reconnect_scenario(demo, ffmpeg, ffprobe, work_dir, scenario))
        else:
            results.append(execute_scenario(demo, ffmpeg, ffprobe, work_dir, scenario))
    summary = {"ok": True, "results": results}
    (work_dir / "results.json").write_text(json.dumps(summary, ensure_ascii=False, indent=2) + "\n",
                                             encoding="utf-8")
    print(json.dumps(summary, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    try:
        main()
    except (AssertionError, OSError, RuntimeError, subprocess.SubprocessError, ValueError) as error:
        raise SystemExit(f"RTSP fault integration check failed: {error}")
