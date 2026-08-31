import sys
import os
import json
import zipfile
import argparse
import re
import subprocess
import shutil
import stat
from pathlib import Path
from typing import Optional

# Switch to project root directory
os.chdir(Path(__file__).resolve().parent.parent)

################################################################################
# Common utility functions
################################################################################

def get_board_type_from_compile_commands() -> Optional[str]:
    """Parse the current compiled BOARD_TYPE from build/compile_commands.json"""
    compile_file = Path("build/compile_commands.json")
    if not compile_file.exists():
        return None
    with compile_file.open(encoding='utf-8') as f:
        data = json.load(f)
    for item in data:
        if not item["file"].endswith("main.cc"):
            continue
        cmd = item["command"]
        if "-DBOARD_TYPE=\\\"" in cmd:
            return cmd.split("-DBOARD_TYPE=\\\"")[1].split("\\\"")[0].strip()
    return None


def get_project_version() -> Optional[str]:
    """Read set(PROJECT_VER "x.y.z") from root CMakeLists.txt"""
    with Path("CMakeLists.txt").open(encoding='utf-8') as f:
        for line in f:
            if line.startswith("set(PROJECT_VER"):
                return line.split("\"")[1]
    return None


def merge_bin() -> None:
    if os.system("idf.py merge-bin") != 0:
        print("merge-bin failed", file=sys.stderr)
        sys.exit(1)


def zip_bin(name: str, version: str) -> None:
    """Zip build/merged-binary.bin to releases/v{version}_{name}.zip"""
    out_dir = Path("releases")
    out_dir.mkdir(exist_ok=True)
    output_path = out_dir / f"v{version}_{name}.zip"

    if output_path.exists():
        output_path.unlink()

    with zipfile.ZipFile(output_path, "w", compression=zipfile.ZIP_DEFLATED) as zipf:
        zipf.write("build/merged-binary.bin", arcname="merged-binary.bin")
    print(f"zip bin to {output_path} done")

def _get_manufacturer(cfg: dict) -> Optional[str]:
    """Read manufacturer from config.json"""
    m = cfg.get("manufacturer")
    if isinstance(m, str) and m.strip():
        return m.strip()
    return None

################################################################################
# board / variant related functions
################################################################################

_BOARDS_DIR = Path("main/boards")

def _collect_variants(config_filename: str = "config.json") -> list[dict[str, str]]:
    """Traverse all boards under main/boards, collect variant information.

    Return example:
        [{"board": "bread-compact-ml307", "name": "bread-compact-ml307", "full_name": "bread-compact-ml307"}, ...]
        [{"board": "waveshare/esp32-p4-nano", "name": "esp32-p4-nano-10.1-a", "full_name": "waveshare-esp32-p4-nano-10.1-a"}, ...]
    """
    variants: list[dict[str, str]] = []
    errors: list[str] = []

    for cfg_path in _BOARDS_DIR.rglob(config_filename):
        board_dir = cfg_path.parent
        if board_dir.name == "common":
            continue
        board = board_dir.relative_to(_BOARDS_DIR).as_posix()

        try:
            with cfg_path.open(encoding='utf-8') as f:
                cfg = json.load(f)

            manufacturer = _get_manufacturer(cfg)

            # Check manufacturer consistency with directory structure
            if "/" in board:
                # Board is in a subdirectory (e.g., waveshare/esp32-p4-nano)
                expected_manufacturer = board.split("/")[0]
                if not manufacturer:
                    errors.append(
                        f"{cfg_path}: Board is in '{expected_manufacturer}/' subdirectory, "
                        f"but config.json is missing \"manufacturer\": \"{expected_manufacturer}\""
                    )
                elif manufacturer != expected_manufacturer:
                    errors.append(
                        f"{cfg_path}: manufacturer mismatch, "
                        f"directory is '{expected_manufacturer}/' but config.json has \"{manufacturer}\""
                    )
            else:
                # Board is directly under boards/ directory
                if manufacturer:
                    errors.append(
                        f"{cfg_path}: Board is not in a manufacturer subdirectory, "
                        f"but config.json defines manufacturer \"{manufacturer}\", "
                        f"please move board to main/boards/{manufacturer}/{board}/"
                    )

            for build in cfg.get("builds", []):
                name = build["name"]
                full_name = f"{manufacturer}-{name}" if manufacturer else name
                variants.append({
                    "board": board, 
                    "name": name,
                    "full_name": full_name
                })

        except Exception as e:
            print(f"[ERROR] Failed to parse {cfg_path}: {e}", file=sys.stderr)

    # Report all errors at once
    if errors:
        print("\n[ERROR] Found manufacturer configuration issues:", file=sys.stderr)
        for err in errors:
            print(f"  - {err}", file=sys.stderr)
        print(file=sys.stderr)
        sys.exit(1)

    return variants



def _find_board_config_candidates(board_type: str) -> list[str]:
    """Find all CONFIG_BOARD_TYPE_xxx candidates for the given board_type."""
    board_leaf = board_type.split("/")[-1]
    pattern = f'set(BOARD_TYPE "{board_leaf}")'

    cmake_file = Path("main/CMakeLists.txt")
    lines = cmake_file.read_text(encoding="utf-8").splitlines()
    candidates: list[str] = []

    for idx, line in enumerate(lines):
        if pattern in line:
            # Found the BOARD_TYPE line, search backwards for the nearest config guard
            for back_idx in range(idx - 1, -1, -1):
                back_line = lines[back_idx]
                if "if(CONFIG_BOARD_TYPE_" in back_line:
                    candidates.append(back_line.strip().split("if(")[1].split(")")[0])
                    break
    return candidates


def _extract_board_config_from_sdkconfig_append(sdkconfig_append: list[str]) -> Optional[str]:
    """Extract explicit CONFIG_BOARD_TYPE_xxx=y from sdkconfig_append, if present."""
    pattern = re.compile(r"^(CONFIG_BOARD_TYPE_[A-Z0-9_]+)=y$")
    matches = []
    for item in sdkconfig_append:
        m = pattern.match(item.strip())
        if m:
            matches.append(m.group(1))
    if not matches:
        return None
    uniq = list(dict.fromkeys(matches))
    if len(uniq) > 1:
        raise ValueError(f"Multiple board type configs found in sdkconfig_append: {uniq}")
    return uniq[0]


def _symbol_supports_target(symbol: str, target: str) -> bool:
    """Check whether Kconfig symbol depends on given target (e.g. esp32c5)."""
    kconfig_file = Path("main/Kconfig.projbuild")
    if not kconfig_file.exists():
        return False

    target_flag = f"IDF_TARGET_{target.upper()}"
    lines = kconfig_file.read_text(encoding="utf-8").splitlines()

    in_symbol = False
    for line in lines:
        stripped = line.strip()
        if stripped.startswith("config "):
            curr_symbol = stripped.split("config ", 1)[1].strip()
            in_symbol = curr_symbol == symbol
            continue
        if in_symbol and stripped.startswith(("config ", "choice ", "endchoice", "menu ", "endmenu")):
            break
        if in_symbol and "depends on" in stripped and target_flag in stripped:
            return True
    return False


def _resolve_board_config(board_type: str, target: str, sdkconfig_append: list[str]) -> str:
    """Resolve CONFIG_BOARD_TYPE_xxx for current board build."""
    explicit = _extract_board_config_from_sdkconfig_append(sdkconfig_append)
    if explicit:
        return explicit

    candidates = _find_board_config_candidates(board_type)
    if not candidates:
        raise ValueError(f"Cannot find board config symbol for {board_type}")
    if len(candidates) == 1:
        return candidates[0]

    by_target = [c for c in candidates if _symbol_supports_target(c, target)]
    if len(by_target) == 1:
        return by_target[0]
    if len(by_target) > 1:
        selected = by_target[0]
        print(
            f"[WARN] Ambiguous board config for {board_type} (target={target}), "
            f"target-matched candidates={by_target}, selecting first: {selected}",
            file=sys.stderr,
        )
        return selected

    target_u = target.upper()
    target_short = target_u.replace("ESP32", "")
    by_name = [
        c for c in candidates
        if target_u in c or f"_{target_short}" in c
    ]
    if len(by_name) == 1:
        return by_name[0]
    if len(by_name) > 1:
        selected = by_name[0]
        print(
            f"[WARN] Ambiguous board config for {board_type} (target={target}), "
            f"name-matched candidates={by_name}, selecting first: {selected}",
            file=sys.stderr,
        )
        return selected

    selected = candidates[0]
    print(
        f"[WARN] Ambiguous board config for {board_type} (target={target}), "
        f"candidates={candidates}, selecting first: {selected}",
        file=sys.stderr,
    )
    return selected


# Kconfig "select" entries are not automatically applied when we simply append
# sdkconfig lines from config.json, so add the required dependencies here to
# mimic menuconfig behaviour.
_AUTO_SELECT_RULES: dict[str, list[str]] = {
    "CONFIG_USE_ESP_BLUFI_WIFI_PROVISIONING": [
        "CONFIG_BT_ENABLED=y",
        "CONFIG_BT_BLUEDROID_ENABLED=y",
        "CONFIG_BT_BLE_42_FEATURES_SUPPORTED=y",
        "CONFIG_BT_BLE_50_FEATURES_SUPPORTED=n",
        "CONFIG_BT_BLE_BLUFI_ENABLE=y",
        "CONFIG_MBEDTLS_DHM_C=y",
    ],
}


def _apply_auto_selects(sdkconfig_append: list[str]) -> list[str]:
    """Apply hardcoded auto-select rules to sdkconfig_append."""
    items: list[str] = []
    existing_keys: set[str] = set()

    def _append_if_missing(entry: str) -> None:
        key = entry.split("=", 1)[0]
        if key not in existing_keys:
            items.append(entry)
            existing_keys.add(key)

    # Preserve original order while tracking keys
    for entry in sdkconfig_append:
        _append_if_missing(entry)

    # Apply auto-select rules
    for key, deps in _AUTO_SELECT_RULES.items():
        for entry in sdkconfig_append:
            name, _, value = entry.partition("=")
            if name == key and value.lower().startswith("y"):
                for dep in deps:
                    _append_if_missing(dep)
                break

    return items

################################################################################
# Check board_type in CMakeLists
################################################################################

def _board_type_exists(board_type: str) -> bool:
    cmake_file = Path("main/CMakeLists.txt").read_text(encoding="utf-8")
    board_leaf = board_type.split("/")[-1]
    pattern = f'set(BOARD_TYPE "{board_leaf}")'
    return pattern in cmake_file


################################################################################
# TinyDrone target selection
################################################################################

_TINY_DRONE_BOARDS: dict[str, str] = {
    "tiny-drone": "TARGET_TINY_DRONE_V1_0",
    "tiny-drone-mini": "TARGET_TINY_DRONE_MINI_V1_0",
}

_TINY_DRONE_PROFILE_DEFAULTS: dict[str, str] = {
    "tiny-drone": "sdkconfig.tiny-drone.defaults",
    "tiny-drone-mini": "sdkconfig.tiny-drone-mini.defaults",
}

_ACTIVE_PROFILE_FILE = ".active-build-profile"


def _get_idf_target_from_kconfig(config_symbol: str) -> str:
    """Resolve a TinyDrone Kconfig symbol to its ESP-IDF target.

    For example, this Kconfig line:
        default TARGET_TINY_DRONE_V1_0 if IDF_TARGET_ESP32S3
    resolves TARGET_TINY_DRONE_V1_0 to esp32s3.
    """
    candidates = (Path("main/Kconfig.projbuild"), Path("Kconfig.projbuild"))
    kconfig_file = next((path for path in candidates if path.exists()), None)
    if kconfig_file is None:
        raise FileNotFoundError("Cannot find main/Kconfig.projbuild or Kconfig.projbuild")

    pattern = re.compile(
        rf"^\s*default\s+{re.escape(config_symbol)}\s+if\s+IDF_TARGET_([A-Z0-9]+)\s*$"
    )
    for line in kconfig_file.read_text(encoding="utf-8").splitlines():
        match = pattern.match(line)
        if match:
            return match.group(1).lower()

    raise ValueError(
        f"Cannot resolve ESP-IDF target for {config_symbol} from {kconfig_file}"
    )


def _sdkconfig_target(sdkconfig: Path) -> Optional[str]:
    """Return CONFIG_IDF_TARGET from a profile sdkconfig, if available."""
    if not sdkconfig.exists():
        return None
    pattern = re.compile(r'^CONFIG_IDF_TARGET="([^"]+)"$')
    for line in sdkconfig.read_text(encoding="utf-8").splitlines():
        match = pattern.match(line.strip())
        if match:
            return match.group(1)
    return None


def _cached_build_directory(build_dir: Path) -> Optional[Path]:
    """Read the absolute build directory recorded by CMake."""
    cache = build_dir / "CMakeCache.txt"
    if not cache.exists():
        return None
    prefix = "CMAKE_CACHEFILE_DIR:INTERNAL="
    for line in cache.read_text(encoding="utf-8", errors="ignore").splitlines():
        if line.startswith(prefix):
            return Path(line[len(prefix):])
    return None


def _idf_py_command() -> list[str]:
    """Use the same Python environment as build.bat to run ESP-IDF."""
    idf_path = os.environ.get("IDF_PATH")
    if not idf_path:
        raise EnvironmentError("IDF_PATH is not set; run build.bat from an ESP-IDF shell")
    idf_py = Path(idf_path) / "tools" / "idf.py"
    if not idf_py.exists():
        raise FileNotFoundError(f"Cannot find {idf_py}")

    # Be tolerant of Windows installations where export.bat activates the
    # compiler but omits locally installed CMake/Ninja directories.
    tools_path = Path(os.environ.get("IDF_TOOLS_PATH", "")) / "tools"
    extra_path: list[str] = []
    for pattern in (
        "cmake/*/bin",
        "ninja/*",
        "xtensa-esp-elf/*/xtensa-esp-elf/bin",
        "riscv32-esp-elf/*/riscv32-esp-elf/bin",
    ):
        matches = sorted(path for path in tools_path.glob(pattern) if path.is_dir())
        if matches:
            extra_path.append(str(matches[-1]))
    if extra_path:
        os.environ["PATH"] = os.pathsep.join(extra_path + [os.environ.get("PATH", "")])

    return [sys.executable, str(idf_py)]


def _is_directory_reparse_point(path: Path) -> bool:
    """Return True for a Windows directory junction or symbolic link."""
    if not path.exists():
        return False
    attributes = getattr(path.lstat(), "st_file_attributes", 0)
    return bool(attributes & stat.FILE_ATTRIBUTE_REPARSE_POINT)


def _activate_build_profile(project_root: Path, profile_root: Path, build_dir: Path,
                            board_name: str) -> None:
    """Make the default project build path follow the last successful profile."""
    default_build = project_root / "build"

    if default_build.exists():
        if _is_directory_reparse_point(default_build):
            # os.rmdir removes the junction itself, never its target contents.
            os.rmdir(default_build)
        else:
            backup = profile_root / "previous-root-build"
            if backup.exists():
                backup = profile_root / f"previous-root-build-{int(os.path.getmtime(default_build))}"
            print(f"Preserving previous root build at: {backup}")
            shutil.move(str(default_build), str(backup))

    result = subprocess.run(
        ["cmd.exe", "/d", "/c", "mklink", "/J", str(default_build), str(build_dir)],
        text=True,
        capture_output=True,
    )
    if result.returncode != 0:
        raise RuntimeError(f"Failed to create build junction: {result.stderr or result.stdout}")

    (project_root / _ACTIVE_PROFILE_FILE).write_text(
        f"board={board_name}\nprofile={profile_root}\nbuild={build_dir}\n",
        encoding="utf-8",
    )
    print(f"Active default build: {default_build} -> {build_dir}")


def build_tiny_drone(board_name: str) -> None:
    """Incrementally build one hardware profile outside the source tree."""
    config_symbol = _TINY_DRONE_BOARDS[board_name]
    target = _get_idf_target_from_kconfig(config_symbol)
    project_root = Path.cwd().resolve()
    profile_root = project_root / f"{board_name}-build"
    build_dir = profile_root / "build"
    profile_sdkconfig = profile_root / "sdkconfig"
    profile_defaults = project_root / _TINY_DRONE_PROFILE_DEFAULTS[board_name]

    profile_root.mkdir(parents=True, exist_ok=True)

    cached_build_dir = _cached_build_directory(build_dir)
    if cached_build_dir is not None and cached_build_dir.resolve() != build_dir.resolve():
        print(f"Build cache moved from {cached_build_dir}; regenerating {build_dir}")
        shutil.rmtree(build_dir)

    # ESP-IDF automatically adds sdkconfig.defaults.<target> next to the base
    # defaults file, so do not list it a second time.
    defaults = [project_root / "sdkconfig.defaults", profile_defaults]
    missing_defaults = [str(path) for path in defaults if not path.exists()]
    if missing_defaults:
        raise FileNotFoundError(f"Missing sdkconfig defaults: {', '.join(missing_defaults)}")

    common_args = [
        "-C", str(project_root),
        "-B", str(build_dir),
        f"-DSDKCONFIG={profile_sdkconfig}",
        f"-DSDKCONFIG_DEFAULTS={';'.join(str(path) for path in defaults)}",
    ]

    ninja_candidates = sorted(
        (Path(os.environ.get("IDF_TOOLS_PATH", "")) / "tools" / "ninja").glob("*/ninja.exe")
    )
    if ninja_candidates:
        common_args.append(f"-DCMAKE_MAKE_PROGRAM={ninja_candidates[-1]}")

    tool_root = Path(os.environ.get("IDF_TOOLS_PATH", "")) / "tools"
    if target == "esp32s3":
        compiler_prefix = "xtensa-esp32s3-elf"
        compiler_candidates = sorted(
            (tool_root / "xtensa-esp-elf").glob(
                f"*/xtensa-esp-elf/bin/{compiler_prefix}-gcc.exe"
            )
        )
    elif target == "esp32c3":
        compiler_prefix = "riscv32-esp-elf"
        compiler_candidates = sorted(
            (tool_root / "riscv32-esp-elf").glob(
                f"*/riscv32-esp-elf/bin/{compiler_prefix}-gcc.exe"
            )
        )
    else:
        compiler_candidates = []

    if compiler_candidates:
        gcc = compiler_candidates[-1]
        compiler_bin = gcc.parent
        common_args.extend([
            f"-DCMAKE_PROGRAM_PATH={compiler_bin};{ninja_candidates[-1].parent if ninja_candidates else ''}",
            f"-DCMAKE_C_COMPILER={gcc}",
            f"-DCMAKE_CXX_COMPILER={compiler_bin / (compiler_prefix + '-g++.exe')}",
            f"-DCMAKE_ASM_COMPILER={gcc}",
        ])

    print(f"Board:  {board_name}")
    print(f"Target: {target}")
    print(f"Config: CONFIG_{config_symbol}=y")
    print(f"Profile: {profile_root}")
    print(f"Build:   {build_dir}")

    os.environ.pop("IDF_TARGET", None)
    idf_py = _idf_py_command()

    # set-target invalidates the CMake cache, so only run it for a new profile
    # or when the profile was previously created for a different chip.
    current_target = _sdkconfig_target(profile_sdkconfig)
    if current_target != target:
        # set-target runs fullclean internally. Remove an incomplete/non-CMake
        # directory left by an interrupted first configuration so fullclean
        # does not refuse to proceed.
        if build_dir.exists() and not (build_dir / "CMakeCache.txt").exists():
            shutil.rmtree(build_dir)
        subprocess.run(idf_py + common_args + ["set-target", target], check=True)

    subprocess.run(idf_py + common_args + ["build"], check=True)
    _activate_build_profile(project_root, profile_root, build_dir, board_name)

################################################################################
# Compile implementation
################################################################################

def release(board_type: str, config_filename: str = "config.json", *, filter_name: Optional[str] = None) -> None:
    """Compile and package all/specified variants of the specified board_type

    Args:
        board_type: directory name under main/boards
        config_filename: config.json name (default: config.json)
        filter_name: if specified, only compile the build["name"] that matches
    """
    cfg_path = _BOARDS_DIR / Path(board_type) / config_filename
    if not cfg_path.exists():
        print(f"[WARN] {cfg_path} does not exist, skipping {board_type}")
        return

    project_version = get_project_version()
    print(f"Project Version: {project_version} ({cfg_path})")

    with cfg_path.open(encoding='utf-8') as f:
        cfg = json.load(f)
    target = cfg["target"]
    manufacturer = _get_manufacturer(cfg)

    builds = cfg.get("builds", [])
    if filter_name:
        builds = [b for b in builds if b["name"] == filter_name]
        if not builds:
            print(f"[ERROR] Variant {filter_name} not found in {board_type}'s {config_filename}", file=sys.stderr)
            sys.exit(1)

    for build in builds:
        name = build["name"]
        board_leaf = board_type.split("/")[-1]

        if board_leaf not in name:
            raise ValueError(f"build.name {name} must contain {board_leaf}")
        
        final_name = f"{manufacturer}-{name}" if manufacturer else name
        output_path = Path("releases") / f"v{project_version}_{final_name}.zip"
        if output_path.exists():
            print(f"Skipping {final_name} because {output_path} already exists")
            continue

        # Process sdkconfig_append
        build_sdkconfig_append = build.get("sdkconfig_append", [])
        explicit_board_cfg = _extract_board_config_from_sdkconfig_append(build_sdkconfig_append)
        if explicit_board_cfg:
            print(
                f"[INFO] Board config explicitly set in config.json: {explicit_board_cfg}, "
                "skip auto-select.",
            )
            sdkconfig_append = list(build_sdkconfig_append)
        else:
            board_type_config = _resolve_board_config(board_type, target, build_sdkconfig_append)
            sdkconfig_append = [f"{board_type_config}=y"]
            sdkconfig_append.extend(build_sdkconfig_append)
        sdkconfig_append = _apply_auto_selects(sdkconfig_append)

        print("-" * 80)
        print(f"name: {final_name}")
        print(f"target: {target}")
        if manufacturer:
            print(f"manufacturer: {manufacturer}")
        for item in sdkconfig_append:
            print(f"sdkconfig_append: {item}")

        os.environ.pop("IDF_TARGET", None)

        # Call set-target
        if os.system(f"idf.py set-target {target}") != 0:
            print("set-target failed", file=sys.stderr)
            sys.exit(1)

        # Append sdkconfig
        with Path("sdkconfig").open("a", encoding='utf-8') as f:
            f.write("\n")
            f.write("# Append by release.py\n")
            for append in sdkconfig_append:
                f.write(f"{append}\n")
        # Build with macro BOARD_NAME defined to name
        if os.system(f"idf.py -DBOARD_NAME={name} -DBOARD_TYPE={board_type} build") != 0:
            print("build failed")
            sys.exit(1)

        # merge-bin
        merge_bin()

        # Zip
        zip_bin(final_name, project_version)

################################################################################
# CLI entry
################################################################################

if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("board", nargs="?", default=None, help="Board type or 'all'")
    parser.add_argument("-c", "--config", default="config.json", help="Config filename (default: config.json)")
    parser.add_argument("--list-boards", action="store_true", help="List all supported boards and variants")
    parser.add_argument("--json", action="store_true", help="Output in JSON format (use with --list-boards)")
    parser.add_argument("--name", help="Variant name to compile (original name without manufacturer prefix)")

    args = parser.parse_args()

    # List mode
    if args.list_boards:
        variants = _collect_variants(config_filename=args.config)
        if args.json:
            print(json.dumps(variants))
        else:
            for v in variants:
                print(f"{v['board']}: {v['name']}")
        sys.exit(0)

    # Current directory firmware packaging mode
    if args.board is None:
        merge_bin()
        curr_board_type = get_board_type_from_compile_commands()
        if curr_board_type is None:
            print("Failed to parse board_type from compile_commands.json", file=sys.stderr)
            sys.exit(1)
        project_ver = get_project_version()
        zip_bin(curr_board_type, project_ver)
        sys.exit(0)

    # Compile mode
    board_type_input: str = args.board
    name_filter: Optional[str] = args.name

    # TinyDrone boards obtain their ESP-IDF chip target directly from
    # Kconfig.projbuild and do not require main/boards/<name>/config.json.
    if board_type_input in _TINY_DRONE_BOARDS:
        try:
            build_tiny_drone(board_type_input)
        except (OSError, ValueError, subprocess.CalledProcessError) as exc:
            print(f"[ERROR] {exc}", file=sys.stderr)
            sys.exit(1)
        sys.exit(0)

    # Check board_type in CMakeLists
    if board_type_input != "all" and not _board_type_exists(board_type_input):
        print(f"[ERROR] board_type {board_type_input} not found in main/CMakeLists.txt", file=sys.stderr)
        sys.exit(1)

    variants_all = _collect_variants(config_filename=args.config)

    # Filter board_type list
    target_board_types: set[str]
    if board_type_input == "all":
        target_board_types = {v["board"] for v in variants_all}
    else:
        target_board_types = {board_type_input}

    for bt in sorted(target_board_types):
        if not _board_type_exists(bt):
            print(f"[ERROR] board_type {bt} not found in main/CMakeLists.txt", file=sys.stderr)
            sys.exit(1)
        cfg_path = _BOARDS_DIR / bt / args.config
        if bt == board_type_input and not cfg_path.exists():
            print(f"Board {bt} has no {args.config} config file, skipping")
            sys.exit(0)
        release(bt, config_filename=args.config, filter_name=name_filter if bt == board_type_input else None)
