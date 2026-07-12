# Lhassa

**LhArchive system service for Amiga**

This is **Lhassa** - an open source implementation of LhA as a system service for Amiga computers. 

## [amigazen project](http://www.amigazen.com)

*A web, suddenly*

*Forty years meditation*

*Minds awaken, free*

**amigazen project** uses modern software development tools and methods to update and rerelease classic Amiga open source software. Projects include a new AWeb, ToolManager, Amiga Python 2, and the ToolKit project  -  a universal SDK for Amiga.

Key to the amigazen project approach is ensuring every project can be built with the same common set of development tools and configurations, so the ToolKit project was created to provide a standard configuration for Amiga development. All *amigazen project* releases are guaranteed to build against the ToolKit standard so that anyone can download and begin contributing straightaway without having to tailor the toolchain for their own setup.

Stefan Boberg, Jim Cooper, David Tritscher, and Sven Ottemann (authors of the classic Amiga LhA releases) are not affiliated with amigazen project. LhA was published as freeware/shareware for AmigaOS from 1991 onward; this tree is an independent reimplementation written from format documentation and reference behaviour, not a derivative of the original LhA sources.

The amigazen project philosophy is based on openness:

*Open* to anyone and everyone  -  *Open* source and free for all  -  *Open* your mind and create!

PRs for all projects are gratefully received at [GitHub](https://github.com/amigazen/). While the focus now is on classic 68k software, it is intended that all amigazen project releases can be ported to other Amiga-like systems including AROS and MorphOS where feasible.

Lhassa is distributed under the [BSD 2-Clause License](LICENSE.md).

## About Lhassa

**LhA** (`.lzh` / `.lha`) is the de facto standard archive format on Amiga platforms from the late 1980s through the 1990s until today. Archives use level 0-2 headers, per-file CRC16 checks, and a family of **lh*** compression methods (lh0 store, lh1-lh7, lzs, lz4, lz5, and others).

**Lhassa** (`lh.lib` + `lha` CLI + `lh.library`) recreates that format in pure **ANSI C (C89)** targeting especially vbcc on 68k AmigaOS but compatible with any recent C compiler on any platform. The `LhA` command included implements the same options as Amiga **LhA 2.15** : `a` adds files, `x` / `e` extract (with or without paths), `l` / `v` list. The library (`liblh.a`, public header `lh.h`) exposes read/write APIs for embedding LhArchive support in other programs.

**lh.library** implements LhArchive as a first class system object - retaining the original four buffer functions (`CreateBuffer`, `DeleteBuffer`, `LhEncode`, `LhDecode`) for backwards compatibility, but now enhanced with a rich API for creating, reading and updating LhA type archives.

## Features

- **LhA 2.15-compatible CLI**  -  same commands, options, and usage line as the classic Amiga archiver (`lha -<options> <command> <archive> ...`).
- **Compress and decompress**  -  lh0 (store), lh1, lh5 (default), lh6, lh7; decompress lh0-lh7, lzs, lz4, lz5; `-z` store only; `-0`  -  `-3` select header level and method.
- **Archive operations**  -  add, delete, concatenate, list (`l` / `v` / `vv`), test, print, extract (`x` / `e`).
- **Header levels**  -  read level 0, 1, and 2 archives (including UNIX extension blocks); write level 0, 1, and 2 headers.
- **Integrity**  -  16-bit CRC on stored data; extract and test report `BAD CRC` but continue through the archive.
- **Progress output**  -  classic LhA-style console progress on extract and test (including `\r` byte counts and `Operation successful.`).
- **Library**  -  `lh.lib` / `lh.h` static link library; `lh.library` shared runtime link library

## Commands

Enter `lha` with no arguments to print the built-in help (from `cli.txt`). Summary:

| Cmd | Action |
|-----|--------|
| `a` | Add file(s) to archive |
| `c` | Concatenate / append archive(s) into one |
| `d` | Delete file(s) from archive |
| `e` | Extract file(s) (names only) |
| `f` | Freshen file(s) in archive |
| `h` | Hunt for differences (archive vs filesystem) |
| `l` / `lq` | List file(s), terse |
| `m` | Move file(s) to archive |
| `p` | Print file(s) to stdout |
| `r` | Replace file(s) in archive |
| `t` | Test file(s) in archive |
| `u` | Update file(s) in archive |
| `v` / `vq` | List file(s), verbose |
| `vv` | List file(s), full detail |
| `x` | Extract with full path |
| `y` | Copy archive with new options |

Common options include `-x` (preserve paths when adding/extracting), `-z` (store uncompressed), `-0` / `-1` / `-2` (header level), `-2` / `-3` (lh5 / lh6 compression), `w=<dir>` (work directory for extract), `-q` (quiet), `-n` / `-N` (suppress progress), and `o5` / `o6` / `o7` (select lh5 / lh6 / lh7 method). The destination directory for extract must end with `/` or `:`.

### Examples

```text
lha a backup.lha Work:Sources/#?
lha -x a project.lha Work:MyProject
lha v project.lha
lha x project.lha RAM:Out/
lha t backup.lha
lha c all.lha part1.lha part2.lha
```

Use `a` to create or append to an archive; do not use `c` for that (`c` is concatenate only).

## Version and compatibility

- **CLI banner:** `LhA Lhassa Version 2.15 68000`  -  command-line compatibility target for Amiga LhA 2.15.
- **Format:** native `.lha` / `.lzh` only. Interchange with archives produced by LhA 2.15, LHa 1.14i, and common PC LHA tools is the design goal for supported methods and header levels.

## Frequently Asked Questions

### What is the difference between LhA, Lhassa, lh.lib and lh.library?

**LhA** refers to the classic archiver and `.lha` format family. **Lhassa** is the name for this reimplementation. The program presents the **LhA 2.15** banner on the command line to signal CLI compatibility. The portable engine is **liblh** (`liblh.a`, `lh.h`). **lh.library** is the AmigaOS shared-library packaging of that engine, replacing the 1990 lh.library with an LHA-native implementation.

### Does Lhassa replace my old LhA executable?

For normal `.lha` / `.lzh` archives, yes  -  use the same commands and options for the implemented subset. Some advanced commands and Amiga integration features are yet to be implemented and are currently no-ops (see below).

### What about multi-volume archives?

Not implemented. Classic LhA supported multivolume options (`-V`, `-Qv`); Lhassa handles single-file archives only.

### What does Lhassa not implement from classic LhA?

Lhassa targets **LHA format compatibility** and a **CLI-compatible front end**. Several LhA 2.15 features are omitted or only stubbed so existing scripts and help text keep working.

**Commands not yet implemented**

| Command | Original purpose |
|---------|------------------|
| `u` | Update archive |
| `f` | Freshen archive |
| `m` | Move files into archive |
| `r` | Replace files |
| `h` | Hunt (compare archive vs filesystem) |
| `y` | Copy archive with new options |

**Compression and archiving**

- **Add progress**  -  classic `Freezing` / `Frozen(%%)` lines during compression are not shown yet (add prints `ADD name` and `Operation successful.`).
- **lh2-lh4, lh8+**  -  recognised in headers; decompression support varies (lh5-lh7, lzs, lz4, lz5 are the main tested paths).
- **Password-protected entries**  -  not supported.
- **Self-extracting archives**  -  not supported.

**Amiga platform integration**

- **Amiga `#?` wildcards**  -  to be added in a future update.
- **Amiga filenotes** as per-file comments (and **`-f`** to ignore them when adding).
- **Preserve file attributes** on extract (`-a`), **clear the archive (A) bit** on extract (`-C`), and **set the A bit** when adding (`-S`).
- **Touch extracted files** with archive timestamps (`-E`).
- **Archive empty directories** (`-e`).
- **Recursive directory scan** (`-r`)  -  accepted for script compatibility; does not walk subdirectories.
- **Collect archives recursively** (`-R`).
- **Work directory** via **`-w`**  -  accepted; use `w=<dir>` for the implemented work-directory path.
- **ENV:LHAOPTS**, **multivolume**, and other platform-only behaviours are omitted.

**No-op command-line options**

These switches are parsed and ignored (or only partially honoured) so LhA 2.15 command lines do not fail. They have no effect in Lhassa today unless noted:

| Option | Original purpose |
|--------|------------------|
| `-a` | Preserve file attributes on extract |
| `-A` | Set archive attributes when adding |
| `-b` | I/O buffer size (KB) |
| `-B` | Keep backup of archive |
| `-c` | Confirm overwrites interactively |
| `-C` | Clear archive (A) bit on extracted files |
| `-d` | Archive date = newest file |
| `-D` | Alternate progress display |
| `-e` | Store empty directories |
| `-E` | Set extracted file date/time from archive |
| `-f` | Ignore Amiga filenotes when adding |
| `-F` | Fast progress display mode |
| `-G` | Only extract newer files |
| `-h` | Disable homedirectories |
| `-i` / `-L` | Read / create file list |
| `-I` | Ignore ENV:LHAOPTS |
| `-k` | Keep partial extractions after errors |
| `-K` | Kill empty directories (move) |
| `-l` / `-u` | Lowercase / uppercase filenames |
| `-m` | No messages for query |
| `-M` | No autoshow files |
| `-o` / `-O` | Add/extract by date |
| `-p` | Pause after loading |
| `-P` | Set Exec task priority |
| `-r` | Recurse into subdirectories |
| `-R` | Collect `.lha` archives recursively |
| `-s` | Add only files without the A bit set |
| `-S` | Set A bit on added files |
| `-t` / `-T` | Only extract new / new & newer files |
| `-V` / `-Qv` | Multivolume archive size / devices |
| `-w` | Temporary work directory (see `w=<dir>`) |
| `-W` | Exclude filenames |
| `-Y` | Store big files below ratio threshold |
| `-Z` | Compress archives inside archive |

**Partially implemented**

| Option / feature | Lhassa behaviour |
|------------------|-----------------|
| `-q` / `lq` / `vq` | Quiet console output |
| `-n` / `-N` | Suppress byte / all progress indicators |
| `-F` | Accepted; fast progress path not fully differentiated |
| `-x` | Preserve path names on add and extract |
| `-z` | Store uncompressed (lh0) |
| `-0` / `-1` / `-2` | Header levels 0, 1, 2 when writing |
| `-2` / `-3` | lh5 / lh6 compression when adding |
| `o5` / `o6` / `o7` | Select lh5 / lh6 / lh7 method |
| `w=<dir>` | Extract work directory |
| File comments | Preserved on rewrite where present in archive headers |
| List totals | Archive date on totals line (Amiga `Lock`/`Examine` where available) |

**Lhassa-only extensions**

| Option | Purpose |
|--------|---------|
| `-v` (as an option flag, not the `v` command) | Verbose header/debug output on stderr |

### Can I contribute?

Yes. Code, testing, documentation, and archive fixtures are welcome. See the repository and [amigazen project](https://github.com/amigazen/) for pull requests.

## Contact

- **GitHub:** https://github.com/amigazen/lhassa/
- **Web:** http://www.amigazen.com/toolkit/ (Amiga browser compatible)
- **amigazen:** toolkit@amigazen.com

## Acknowledgements

*Amiga* is a trademark of **Amiga Inc**.

**LhA** was developed by **Stefan Boberg**, **Jim Cooper**, **David Tritscher**, **Sven Ottemann**, and the wider LhA for UNIX community (Yooichi Tagawa, Nobutaka Watazaki, and others).