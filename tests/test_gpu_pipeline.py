#!/usr/bin/env python3
"""Воспроизводимая GPU-проверка MP4-конвейера без сторонних Python-пакетов."""

import argparse
import json
import re
import subprocess
from pathlib import Path
from decimal import Decimal


WIDTH = 640
HEIGHT = 360
FRAME_RATE = 10
FRAME_COUNT = 10
FRAME_DURATION = 0.1


def run(command):
    result = subprocess.run(command, check=False, text=True, capture_output=True)
    if result.returncode != 0:
        raise RuntimeError(
            f"Команда завершилась с кодом {result.returncode}: {command}\n"
            f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}"
        )
    return result


def generate_video(ffmpeg, destination, frames, duplicate_pts=False):
    setpts = "setpts='if(eq(N,1),0,N/(10*TB))'" if duplicate_pts else "null"
    run([
        ffmpeg, "-hide_banner", "-loglevel", "error", "-y",
        "-f", "lavfi", "-i", f"testsrc2=size={WIDTH}x{HEIGHT}:rate={FRAME_RATE}",
        "-vf", f"{setpts},setsar=4/3,setparams=range=full:color_primaries=bt709:color_trc=bt709:colorspace=bt709",
        "-frames:v", str(frames), "-fps_mode", "passthrough",
        "-c:v", "libx264", "-bf", "0", "-pix_fmt", "yuv420p",
        "-color_range", "pc", "-colorspace", "bt709",
        "-color_primaries", "bt709", "-color_trc", "bt709",
        str(destination),
    ])


def probe(ffprobe, path):
    result = run([
        ffprobe, "-v", "error", "-show_frames", "-show_streams",
        "-select_streams", "v:0", "-of", "json", str(path),
    ])
    parsed = json.loads(result.stdout)
    streams = parsed.get("streams", [])
    if len(streams) != 1:
        raise AssertionError(f"Ожидался один видеопоток в {path}, получено: {len(streams)}")
    frames = parsed.get("frames", [])
    return streams[0], frames


def frame_times_us(frames, path):
    times = []
    for frame in frames:
        value = frame.get("best_effort_timestamp_time", frame.get("pts_time"))
        if value is None:
            raise AssertionError(f"В кадре отсутствует PTS: {path}")
        times.append(int(Decimal(value) * 1_000_000))
    return times


def assert_video(ffmpeg, ffprobe, path, expected_count, expected_duration):
    stream, frames = probe(ffprobe, path)
    assert len(frames) == expected_count, f"{path}: кадров {len(frames)}, ожидалось {expected_count}"
    assert (stream.get("width"), stream.get("height")) == (WIDTH, HEIGHT), stream
    assert stream.get("codec_name") == "h264", stream
    assert stream.get("sample_aspect_ratio") == "4:3", stream
    assert stream.get("color_range") == "pc", stream
    for field in ("color_space", "color_primaries", "color_transfer"):
        assert stream.get(field) == "bt709", f"{path}: {field}={stream.get(field)!r}"
    duration = float(stream["duration"])
    assert abs(duration - expected_duration) <= 0.000001, (
        f"{path}: duration={duration}, ожидалось {expected_duration}"
    )
    run([ffmpeg, "-hide_banner", "-loglevel", "error", "-xerror", "-i", str(path), "-f", "null", "-"])
    return frame_times_us(frames, path)


def transcode(demo, source, destination):
    # Обе очереди вмещают весь fixture; их общий резерв укладывается в NVDEC на тестовом GPU.
    return run([demo, "--queue-depth", "10", "--write-queue-depth", "10", "--transcode", str(source), str(destination)])


def check_corrected_count(output, expected):
    # Счётчик печатает Writer при finish; разрешаем различия в локализованной подписи.
    match = re.search(r"(?:correctedTimestamps|скорректировано\s+timestamps?)\D*(\d+)", output, re.IGNORECASE)
    if not match:
        raise AssertionError(f"В выводе camera_demo нет счётчика исправлений timestamp:\n{output}")
    actual = int(match.group(1))
    assert actual == expected, f"Исправлено timestamp: {actual}, ожидалось {expected}"


def main():
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

    source = work_dir / "source_10_frames.mp4"
    output = work_dir / "transcoded_10_frames.mp4"
    one_source = work_dir / "source_1_frame.mp4"
    one_output = work_dir / "transcoded_1_frame.mp4"
    duplicate_source = work_dir / "source_duplicate_pts.mkv"
    duplicate_output = work_dir / "transcoded_duplicate_pts.mp4"

    generate_video(ffmpeg, source, FRAME_COUNT)
    source_times = assert_video(ffmpeg, ffprobe, source, FRAME_COUNT, 1.0)
    assert source_times == [index * 100_000 for index in range(FRAME_COUNT)], source_times
    result = transcode(demo, source, output)
    output_times = assert_video(ffmpeg, ffprobe, output, FRAME_COUNT, 1.0)
    assert output_times == source_times, (source_times, output_times)
    assert "Обработано: 10 кадров" in result.stdout, result.stdout

    generate_video(ffmpeg, one_source, 1)
    one_source_times = assert_video(ffmpeg, ffprobe, one_source, 1, FRAME_DURATION)
    one_result = transcode(demo, one_source, one_output)
    one_output_times = assert_video(ffmpeg, ffprobe, one_output, 1, FRAME_DURATION)
    assert one_output_times == one_source_times, (one_source_times, one_output_times)
    assert "Обработано: 1 кадров" in one_result.stdout, one_result.stdout

    generate_video(ffmpeg, duplicate_source, FRAME_COUNT, duplicate_pts=True)
    duplicate_input_times = frame_times_us(probe(ffprobe, duplicate_source)[1], duplicate_source)
    duplicates = sum(current <= previous for previous, current in zip(duplicate_input_times, duplicate_input_times[1:]))
    if duplicates == 0:
        raise AssertionError(f"FFmpeg fixture не сохранил повторяющиеся PTS: {duplicate_input_times}")
    duplicate_result = transcode(demo, duplicate_source, duplicate_output)
    duplicate_output_times = assert_video(ffmpeg, ffprobe, duplicate_output, FRAME_COUNT, 1.0)
    expected_times = []
    for source_time in duplicate_input_times:
        expected_times.append(source_time if not expected_times else max(source_time, expected_times[-1] + 1))
    assert duplicate_output_times == expected_times, (duplicate_input_times, expected_times, duplicate_output_times)
    check_corrected_count(duplicate_result.stdout + duplicate_result.stderr, duplicates)

    print("GPU MP4 integration checks passed: 10 frames, one frame, duplicate PTS, metadata and full decode")


if __name__ == "__main__":
    try:
        main()
    except (AssertionError, RuntimeError, KeyError, ValueError) as error:
        raise SystemExit(f"GPU integration check failed: {error}")
