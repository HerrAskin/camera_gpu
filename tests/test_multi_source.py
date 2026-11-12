#!/usr/bin/env python3
"""Проверяет пакетный CPU transcode нескольких локальных источников."""

import argparse
import math
import re
import subprocess
import tempfile
from pathlib import Path


WIDTH = 640
HEIGHT = 360
FRAME_COUNT = 10
PIPELINE_METRICS = re.compile(
    r"^\[Pipeline\] Выход: (?P<path>.*); кадров: (?P<frames>\d+); "
    r"время: (?P<seconds>[0-9]+\.[0-9]{6}) с; средний FPS: "
    r"(?P<fps>[0-9]+\.[0-9]{6}); пропущено очередью: (?P<dropped>[0-9]+)$",
    re.MULTILINE,
)
SESSION_METRICS = re.compile(
    r"^\[Session\] Источников: (?P<sources>\d+); кадров: (?P<frames>\d+); "
    r"время: (?P<seconds>[0-9]+\.[0-9]{6}) с; суммарный FPS: "
    r"(?P<fps>[0-9]+\.[0-9]{6})$",
    re.MULTILINE,
)


def run(command: list[str], expected: int = 0) -> subprocess.CompletedProcess[str]:
    result = subprocess.run(command, check=False, text=True, capture_output=True, timeout=30, cwd=Path(__file__).resolve().parents[1])
    assert result.returncode == expected, (
        f"Ожидался код {expected}, получен {result.returncode}: {command}\n"
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
        ffprobe, "-v", "error", "-count_frames", "-select_streams", "v:0",
        "-show_entries", "stream=width,height,nb_read_frames", "-of", "default=nw=1", str(path),
    ])
    assert f"width={WIDTH}" in probe.stdout, probe.stdout
    assert f"height={HEIGHT}" in probe.stdout, probe.stdout
    assert f"nb_read_frames={FRAME_COUNT}" in probe.stdout, probe.stdout
    run([ffmpeg, "-hide_banner", "-loglevel", "error", "-xerror", "-i", str(path), "-f", "null", "-"])


def verify_metrics(result: subprocess.CompletedProcess[str], outputs: dict[str, int], total_frames: int) -> None:
    text = result.stdout + result.stderr
    pipelines = list(PIPELINE_METRICS.finditer(text))
    assert len(pipelines) == len(outputs), text
    for match in pipelines:
        path = match.group("path")
        assert path in outputs, f"Неожиданный путь в метриках: {path}\n{text}"
        frames = int(match.group("frames"))
        seconds = float(match.group("seconds"))
        fps = float(match.group("fps"))
        dropped = int(match.group("dropped"))
        assert frames == outputs[path], match.group(0)
        assert math.isfinite(seconds) and seconds > 0, match.group(0)
        assert math.isfinite(fps) and fps >= 0, match.group(0)
        assert math.isfinite(dropped) and dropped >= 0, match.group(0)
        expected_fps = frames / seconds
        if frames:
            assert math.isclose(fps, expected_fps, rel_tol=0.01), match.group(0)
        else:
            assert fps == 0, match.group(0)

    sessions = list(SESSION_METRICS.finditer(text))
    assert len(sessions) == 1, text
    session = sessions[0]
    seconds = float(session.group("seconds"))
    fps = float(session.group("fps"))
    assert int(session.group("sources")) == len(outputs), session.group(0)
    assert int(session.group("frames")) == total_frames, session.group(0)
    assert math.isfinite(seconds) and seconds > 0, session.group(0)
    assert math.isfinite(fps) and fps >= 0, session.group(0)
    if total_frames:
        assert math.isclose(fps, total_frames / seconds, rel_tol=0.01), session.group(0)
    else:
        assert fps == 0, session.group(0)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--demo", required=True)
    parser.add_argument("--ffmpeg", required=True)
    parser.add_argument("--ffprobe", required=True)
    parser.add_argument("--work-dir", required=True)
    args = parser.parse_args()

    demo = str(Path(args.demo).resolve())
    cpu = [demo, "--cpu", "--queue-depth", "10", "--write-queue-depth", "10"]
    ffmpeg = str(Path(args.ffmpeg).resolve())
    ffprobe = str(Path(args.ffprobe).resolve())
    work_dir = Path(args.work_dir).resolve()
    work_dir.mkdir(parents=True, exist_ok=True)
    run_dir = Path(tempfile.mkdtemp(prefix="multi_source_", dir=work_dir))

    help_result = run([demo, "--help"])
    assert "--source" in help_result.stdout, help_result.stdout
    noargs_result = run([demo])
    assert "usage" in noargs_result.stdout.lower() or "camera_demo" in noargs_result.stdout, noargs_result.stdout

    first_dir = run_dir / "one"
    second_dir = run_dir / "two"
    first_dir.mkdir()
    second_dir.mkdir()
    first = first_dir / "video1.mp4"
    second = second_dir / "video2.mp4"
    same_stem = second_dir / "video1.mp4"
    generate_fixture(ffmpeg, first)
    generate_fixture(ffmpeg, second)
    generate_fixture(ffmpeg, same_stem)

    output_dir = run_dir / "outputs"
    multi_result = run([
        *cpu,
        "--source", str(first), "--source", str(second), "--output-dir", str(output_dir),
    ])
    verify_metrics(multi_result, {
        str(output_dir / "video1_out.mp4"): FRAME_COUNT,
        str(output_dir / "video2_out.mp4"): FRAME_COUNT,
    }, FRAME_COUNT * 2)
    for output in (output_dir / "video1_out.mp4", output_dir / "video2_out.mp4"):
        verify_output(ffmpeg, ffprobe, output)

    lossless = run_dir / "lossless.mp4"
    run([demo, "--cpu", "--keep-all-frames", "--queue-depth", "1", "--write-queue-depth", "0", "--source", str(first), "--output", str(lossless)])
    verify_output(ffmpeg, ffprobe, lossless)

    duplicate_dir = run_dir / "duplicate_stems"
    run([
        *cpu, "--source", str(first), "--source", str(same_stem),
        "--output-dir", str(duplicate_dir),
    ])
    verify_output(ffmpeg, ffprobe, duplicate_dir / "video1_out.mp4")
    verify_output(ffmpeg, ffprobe, duplicate_dir / "video1_out_2.mp4")

    explicit = run_dir / "chosen-name.mp4"
    explicit_dir = run_dir / "explicit_second"
    run([
        *cpu, "--source", str(first), "--output", str(explicit),
        "--source", str(second), "--output-dir", str(explicit_dir),
    ])
    verify_output(ffmpeg, ffprobe, explicit)
    verify_output(ffmpeg, ffprobe, explicit_dir / "video2_out.mp4")

    default_source = run_dir / f"{run_dir.name}_default.mp4"
    generate_fixture(ffmpeg, default_source)
    default_result = run([*cpu, "--source", str(default_source)])
    assert "Каталог записи по умолчанию: tmp/recordings" in default_result.stdout
    default_output = Path(__file__).resolve().parents[1] / "tmp" / "recordings" / f"{default_source.stem}_out.mp4"
    verify_output(ffmpeg, ffprobe, default_output)

    bad_source = run_dir / "missing.mp4"
    partial_dir = run_dir / "partial"
    partial = run([
        *cpu, "--source", str(bad_source), "--source", str(second),
        "--output-dir", str(partial_dir),
    ], expected=1)
    verify_metrics(partial, {
        str(partial_dir / "missing_out.mp4"): 0,
        str(partial_dir / "video2_out.mp4"): FRAME_COUNT,
    }, FRAME_COUNT)
    assert (partial_dir / "video2_out.mp4").is_file(), partial.stderr
    verify_output(ffmpeg, ffprobe, partial_dir / "video2_out.mp4")

    for flags in (["--source"], ["--output", "tmp/no-source.mp4"], ["--source", str(first), "--output", "a.mp4", "--output", "b.mp4"]):
        run([demo, *flags], expected=1)

    original = first.read_bytes()
    same_path = run([*cpu, "--source", str(first), "--output", str(first)], expected=1)
    assert first.read_bytes() == original, "Отклонение одинаковых input/output повредило источник"
    assert "".join(same_path.stdout + same_path.stderr).strip(), "Не сообщена ошибка одинаковых input/output"


if __name__ == "__main__":
    main()
