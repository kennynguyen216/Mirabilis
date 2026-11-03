"""Fetch Intel's original Sponza glTF, 4K textures and Colorful Curtains."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import io
import json
import os
from pathlib import Path, PurePosixPath
import shutil
import subprocess
import threading
import zipfile
import zlib
from urllib.parse import unquote

SOURCE = "https://www.intel.com/content/www/us/en/developer/topic-technology/graphics-research/samples.html"
PACKAGES = {
    "main": ("https://cdrdv2.intel.com/v1/dl/getContent/830833", 3987608266),
    "curtains": ("https://cdrdv2.intel.com/v1/dl/getContent/726650", 786898766),
}


class RemoteZip(io.RawIOBase):
    def __init__(self, url, size):
        self.url, self.size, self.position = url, size, 0
        self.cache = (0, b"")

    def seekable(self): return True
    def readable(self): return True
    def tell(self): return self.position

    def seek(self, offset, whence=0):
        if whence not in (0, 1, 2): raise ValueError("Invalid seek origin")
        self.position = offset if whence == 0 else self.position + offset if whence == 1 else self.size + offset
        if self.position < 0: raise ValueError("Negative seek")
        return self.position

    def read(self, count=-1):
        if count < 0: count = self.size - self.position
        count = min(count, self.size - self.position)
        if count <= 0: return b""
        start, data = self.cache
        if not (start <= self.position and self.position + count <= start + len(data)):
            start = self.position
            length = min(max(count, 65536), self.size - start)
            request = subprocess.run([
                "curl.exe", "--fail", "--silent", "--show-error", "--location", "--retry", "2",
                "--connect-timeout", "15", "--speed-time", "30", "--speed-limit", "1024",
                "--max-time", "120", "--range", f"{start}-{start + length - 1}",
                "--max-filesize", str(length), self.url], capture_output=True)
            if request.returncode: raise OSError(request.stderr.decode(errors="replace"))
            data = request.stdout
            if len(data) != length: raise OSError("Server did not honor the ZIP byte range")
            self.cache = (start, data)
        result = data[self.position - start:self.position - start + count]
        self.position += len(result)
        return result


def checksum(path):
    crc = 0
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(4 * 1024 * 1024), b""):
            crc = zlib.crc32(block, crc)
    return crc


def safe_relative(name):
    relative = PurePosixPath(unquote(name).replace("\\", "/"))
    if relative.is_absolute() or ".." in relative.parts or any(":" in part or "\0" in part for part in relative.parts):
        raise ValueError("Asset path escapes its package")
    return relative


def selected_files(archive):
    model = next(info for info in archive.infolist() if info.filename.endswith(".gltf"))
    gltf = json.loads(archive.read(model))
    prefix = PurePosixPath(model.filename).parent
    references = [model.filename, (prefix / "credits_license.txt").as_posix()]
    for entry in [*gltf.get("buffers", []), *gltf.get("images", [])]:
        relative = safe_relative(entry["uri"])
        references.append((prefix / relative).as_posix())
    return [archive.getinfo(name) for name in dict.fromkeys(references)]


def write_preview_profile(target, optimized):
    """Preserve original downloads; reference 2K data maps in the optional 6GB profile."""
    source = target / "main_sponza/NewSponza_Main_glTF_003.gltf"
    model = json.loads(source.read_text(encoding="utf-8"))
    colors = set()
    for material in model.get("materials", []):
        info = material.get("pbrMetallicRoughness", {}).get("baseColorTexture")
        if info is not None:
            colors.add(model["textures"][info["index"]]["source"])
    directory = target / "runtime_6gb"
    directory.mkdir(exist_ok=True)
    for buffer in model["buffers"]:
        buffer["uri"] = os.path.relpath(source.parent / buffer["uri"], directory).replace("\\", "/")
    for index, image in enumerate(model["images"]):
        path = source.parent / image["uri"] if index in colors else optimized / image["uri"]
        if not path.is_file(): raise FileNotFoundError(f"Preview needs the existing 2K data map: {path}")
        image["uri"] = os.path.relpath(path, directory).replace("\\", "/")
    model.setdefault("extras", {})["mirabilisTextureProfile"] = "4K color; existing 2K normal/roughness; original 4K curtains"
    (directory / "Sponza_4K_Color.gltf").write_text(json.dumps(model, indent=2), encoding="utf-8")
    print("Created optional 6GB preview: 4K color and curtains; 2K data maps", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=Path(__file__).resolve().parent.parent / "assets/sponza_hq")
    args = parser.parse_args()
    target = args.output.resolve()
    target.mkdir(parents=True, exist_ok=True)
    plans = []
    for package, (url, size) in PACKAGES.items():
        with zipfile.ZipFile(RemoteZip(url, size)) as archive:
            for info in selected_files(archive):
                relative = safe_relative(info.filename)
                destination = target.joinpath(*relative.parts)
                if not destination.resolve().is_relative_to(target): raise ValueError("Asset directory escapes destination")
                if destination.exists():
                    if destination.stat().st_size == info.file_size and checksum(destination) == info.CRC:
                        print("Verified", relative, flush=True)
                        continue
                    raise FileExistsError(f"Existing asset differs; use another destination: {destination}")
                plans.append((package, info.filename, info.file_size, info.CRC))
    needed = sum(item[2] for item in plans)
    if shutil.disk_usage(target).free < needed + 512 * 1024 * 1024:
        raise OSError(f"Need {needed / 2**30:.2f} GiB plus 512 MiB of free space")
    print(f"Fetching {len(plans)} files, {needed / 2**30:.2f} GiB; no texture resizing or mesh simplification", flush=True)
    local = threading.local()

    def fetch(item):
        package, name, size, crc = item
        if not hasattr(local, "archives"): local.archives = {}
        if package not in local.archives:
            local.archives[package] = zipfile.ZipFile(RemoteZip(*PACKAGES[package]))
        archive = local.archives[package]
        destination = target.joinpath(*PurePosixPath(name).parts)
        destination.parent.mkdir(parents=True, exist_ok=True)
        partial = destination.with_name(destination.name + ".partial")
        with archive.open(name) as source, partial.open("wb") as output:
            shutil.copyfileobj(source, output, length=32 * 1024 * 1024)
        if partial.stat().st_size != size or checksum(partial) != crc:
            raise OSError(f"Integrity check failed: {name}")
        partial.replace(destination)
        print("Installed", name, f"({size / 1048576:.1f} MiB)", flush=True)

    with ThreadPoolExecutor(max_workers=3) as workers:
        list(workers.map(fetch, plans))
    manifest = {"source": SOURCE, "packages": {key: value[0] for key, value in PACKAGES.items()},
                "note": "Original glTF assets and PNG textures; archive CRCs verified; no downsampling"}
    (target / "source.json").write_text(json.dumps(manifest, indent=2), encoding="utf-8")
    optimized = Path(__file__).resolve().parent.parent / "assets/sponza_optimized"
    if optimized.is_dir(): write_preview_profile(target, optimized)
    print("SPONZA HQ DOWNLOAD COMPLETE", flush=True)


if __name__ == "__main__": main()
