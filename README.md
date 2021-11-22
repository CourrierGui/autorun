# Autorun

Autorun allows you to run arbitrary commands when an inotify event happens on a
file or inside of a directory.

## Usage

```
autorun [--file|-f <filenames>] [--dir|-d <dirnames>] <cmd>

    --help|-h    display this message
    --version|-v current version
    --file|-f    name of the files whose events will trigger <cmd>
    --dir|-d     all events on files and directories inside <dirnames> will trigger <cmd>
                 (autorun will watch . by default)
    <cmd>        the command that will be run when an event is detected
```

For example:
```bash
autorun --file autorun.cpp meson.build config.h.in -- ninja -C build
autorun --dir test -- echo "An event occured inside test directory"
```

Any of the following commands will trigger the command `<cmd>`:
```bash
echo "// 10" >> autorun.cpp
mkdir test/d
touch test/d/f
echo 10 > test/d/f
```

## Installation

```
$ git clone https://github.com/CourrierGui/autorun
$ cd autorun
$ meson setup build
$ ninja -C build
# ninja -C build install
```
