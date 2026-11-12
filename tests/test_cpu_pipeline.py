#!/usr/bin/env python3
"""Проверяет CPU decode/encode полного конвейера на локальном FFmpeg fixture."""

import argparse
import json
import subprocess
from decimal import Decimal
from pathlib import Path


WIDTH = 640
HEIGHT = 360
FRAME_COUNT = 10
FRAME_DURATION_US = 100_000


def run(command: list[str]) -> subprocess.CompletedProcess[str]:
    result = subprocess.run(command, check=False, text=True, capture_output=True, timeout=30)
    if result.returncode != 0:
        raise RuntimeError(
            f"Команда завершилась с кодом {result.returncode}: {command}\n"
            f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}"
        )
    return result


def generate_fixture(ffmpeg: str, destination: Path) -> None:
    run([
        ffmpeg, "-hide_banner", "-loglevel", "error", "-y",
        "-f", "lavfi", "-i", f"testsrc=size={WIDTH}x{HEIGHT}:rate=10",
        "-frames:v", str(FRAME_COUNT), "-c:v", "libx264", "-bf", "0",
        "-pix_fmt", "yuv420p", str(destination),
    ])


def verify_output(ffmpeg: str, ffprobe: str, path: Path) -> None:
    probe = run([
        ffprobe, "-v", "error", "-show_frames", "-show_streams",
        "-select_streams", "v:0", "-of", "json", str(path),
    ])
    metadata = json.loads(probe.stdout)
    streams = metadata.get("streams", [])
    frames = metadata.get("frames", [])
    assert len(streams) == 1, f"Ожидался один видеопоток: {streams}"
    stream = streams[0]
    assert len(frames) == FRAME_COUNT, f"Кадров {len(frames)}, ожидалось {FRAME_COUNT}"
    assert (stream.get("width"), stream.get("height")) == (WIDTH, HEIGHT), stream
    assert stream.get("codec_name") == "h264", stream
    times = []
    for frame in frames:
        timestamp = frame.get("best_effort_timestamp_time") or frame.get("pts_time")
        assert timestamp is not None, f"В кадре отсутствует PTS: {frame}"
        times.append(int(Decimal(timestamp) * 1_000_000))
    expected_times = [index * FRAME_DURATION_US for index in range(FRAME_COUNT)]
    assert times == expected_times, f"PTS {times}, ожидались {expected_times}"
    assert abs(float(stream["duration"]) - 1.0) <= 0.000001, stream
    run([ffmpeg, "-hide_banner", "-loglevel", "error", "-xerror", "-i", str(path), "-f", "null", "-"])


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--demo", required=True)
    parser.add_argument("--ffmpeg", required=True)
    parser.add_argument("--ffprobe", required=True)
    parser.add_argument("--work-dir", required=True)
    args = parser.parse_args()

    demo = str(Path(args.demo).resolve())
    ffmpeg = str(Path(args.ffmpeg).resolve())
    ffprobe = str(Path(args.ffprobe).resolve())
    work_dir = Path(args.work_dir).resolve()
    work_dir.mkdir(parents=True, exist_ok=True)
    source = work_dir / "cpu_source_10_frames.mp4"
    output = work_dir / "cpu_transcoded_10_frames.mp4"

    generate_fixture(ffmpeg, source)
    result = run([
        demo, "--cpu", "--queue-depth", "10", "--write-queue-depth", "10",
        "--transcode", str(source), str(output),
    ])
    combined_output = result.stdout + result.stderr
    assert "Обработано: 10 кадров" in combined_output, combined_output
    lowered_output = combined_output.lower()
    assert "cpu" in lowered_output, f"Явный CPU mode не отражён в логах:\n{combined_output}"
    assert not any(marker in lowered_output for marker in ("cuda decoder", "nvdec decoder", "gpu decode")), combined_output
    verify_output(ffmpeg, ffprobe, output)


if __name__ == "__main__":
    main()
