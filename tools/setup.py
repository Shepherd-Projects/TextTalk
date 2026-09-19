#!/usr/bin/env python3
"""Fetch the pinned native Windows CUDA SDK, Nemotron GGUF, and redistribution notices."""

import argparse
import concurrent.futures
import json
import shutil
import stat
import sys
import urllib.request
import zipfile
from pathlib import Path, PurePosixPath

SDK_NAME = "nemo-speech-0.1.0-windows-x86_64-cuda.zip"
SDK_URL = f"https://github.com/NVIDIA/NeMo-Speech.cpp/releases/download/v0.1.0/{SDK_NAME}"
SDK_SIZE = 106044768
SDK_REQUIRED = (
    "include/nemo_speech/asr.h", "lib/nemo_speech_asr_c.lib",
    "bin/nemo_speech_asr_c.dll", "bin/nemo_speech_asr.dll",
    "bin/ggml.dll", "bin/ggml-base.dll", "bin/ggml-cpu.dll", "bin/ggml-cuda.dll",
    "bin/cublas64_13.dll", "bin/msvcp140.dll", "bin/vcruntime140.dll",
    "bin/vcruntime140_1.dll", "bin/vcomp140.dll",
    "share/licenses/nemo-speech/LICENSE", "share/licenses/nemo-speech/NOTICE",
    "share/licenses/nemo-speech/THIRD_PARTY_NOTICES.txt",
)
MODEL_REPO = "nvidia/nemotron-speech-streaming-en-0.6b"
MODEL_REVISION = "ebe59e5a817142986528bbbee5dba8db7b38ed50"
MODEL_NAME = "nemotron-speech-streaming-en-0.6b.q8_0.gguf"
MODEL_SIZE = 699872960
MODEL_URL = f"https://huggingface.co/{MODEL_REPO}/resolve/{MODEL_REVISION}/{MODEL_NAME}"


def download(url, destination, expected_size=None):
    if destination.is_file() and destination.stat().st_size > 0:
        if expected_size is None or destination.stat().st_size == expected_size:
            return destination
    destination.parent.mkdir(parents=True, exist_ok=True)
    partial = destination.with_name(destination.name + ".part")
    request = urllib.request.Request(url, headers={"User-Agent": "TalkText-setup/2"})
    print(f"Downloading {destination.name}", flush=True)
    try:
        with urllib.request.urlopen(request, timeout=120) as response, partial.open("wb") as output:
            response_size = response.headers.get("Content-Length")
            shutil.copyfileobj(response, output, length=1024 * 1024)
        actual_size = partial.stat().st_size
        required_size = expected_size if expected_size is not None else int(response_size or actual_size)
        if actual_size == 0 or actual_size != required_size:
            raise RuntimeError(f"Incomplete download: {destination.name}: {actual_size}/{required_size} bytes")
        partial.replace(destination)
    finally:
        partial.unlink(missing_ok=True)
    return destination


def unpack_sdk(archive_path, destination):
    files = {}
    with zipfile.ZipFile(archive_path) as archive:
        for member in archive.infolist():
            path = PurePosixPath(member.filename)
            if path.is_absolute() or ".." in path.parts or any(":" in part or "\\" in part for part in path.parts):
                raise RuntimeError(f"Unsafe archive path: {member.filename}")
            if stat.S_ISLNK(member.external_attr >> 16):
                raise RuntimeError(f"Archive contains symlink: {member.filename}")
            if member.is_dir():
                continue
            is_notice = any(part.lower().startswith(("license", "notice", "copyright", "third_party", "third-party")) for part in path.parts)
            if path.suffix.lower() not in (".h", ".hpp", ".lib", ".dll", ".cmake", ".pc") and not is_notice:
                continue
            if path.suffix.lower() == ".md":
                path = path.with_suffix(".txt")
            output = destination.joinpath(*path.parts)
            if not output.resolve().is_relative_to(destination.resolve()):
                raise RuntimeError(f"Archive destination escapes SDK: {output}")
            files[path.as_posix()] = member.file_size
            if output.is_file() and output.stat().st_size == member.file_size:
                continue
            output.parent.mkdir(parents=True, exist_ok=True)
            partial = output.with_name(output.name + ".part")
            try:
                with archive.open(member) as source, partial.open("wb") as target:
                    shutil.copyfileobj(source, target, length=1024 * 1024)
                partial.replace(output)
            finally:
                partial.unlink(missing_ok=True)
    for name in SDK_REQUIRED:
        if not files.get(name):
            raise RuntimeError(f"Pinned SDK lacks required file: {name}")
    manifest = {"url": SDK_URL, "release": "v0.1.0", "archive_bytes": SDK_SIZE, "files": files}
    (destination / "asset-manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    print(f"Installed SDK: {destination}", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1], help="TalkText source/package root")
    args = parser.parse_args()
    root = args.root.resolve()
    sdk = root / "third_party/nemo-speech"
    model = root / "models/nemotron"
    with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
        sdk_job = pool.submit(download, SDK_URL, sdk / "downloads" / SDK_NAME, SDK_SIZE)
        model_job = pool.submit(download, MODEL_URL, model / MODEL_NAME, MODEL_SIZE)
        unpack_sdk(sdk_job.result(), sdk)
        model_job.result()
    with (model / MODEL_NAME).open("rb") as source:
        if source.read(4) != b"GGUF":
            raise RuntimeError("Pinned model is not a GGUF file")
    download(f"https://huggingface.co/{MODEL_REPO}/resolve/{MODEL_REVISION}/README.md", model / "MODEL-CARD.txt", 25987)
    license_dir = root / "third_party/licenses"
    download("https://www.nvidia.com/en-us/agreements/enterprise-software/nvidia-open-model-license.md", license_dir / "nemo-model-LICENSE.txt")
    download("https://www.nvidia.com/en-us/agreements/trustworthy-ai/terms.md", license_dir / "nemo-model-Trustworthy-AI.txt")
    # The Windows archive omits these terms despite including CUDA and VC++ code.
    download("https://docs.nvidia.com/cuda/eula/index.html.md", license_dir / "nemo-CUDA-LICENSE.txt")
    download("https://visualstudio.microsoft.com/wp-content/uploads/2021/09/Visual-C-Runtime-2015-2022-License-1.docx",
             license_dir / "nemo-Visual-C-Runtime-LICENSE.docx")
    (model / "Notice.txt").write_text("Licensed by NVIDIA Corporation under the NVIDIA Open Model License\n", encoding="utf-8")
    manifest = {"repository": MODEL_REPO, "revision": MODEL_REVISION, "last_modified": "2026-08-05T21:45:11Z", "file": MODEL_NAME, "bytes": MODEL_SIZE, "url": MODEL_URL}
    (model / "asset-manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    print("Dependencies ready. SDK: third_party/nemo-speech. Model: models/nemotron.")


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, RuntimeError, zipfile.BadZipFile) as error:
        print(f"Setup failed: {error}", file=sys.stderr)
        sys.exit(1)
