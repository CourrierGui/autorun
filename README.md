# Autorun

Autorun allows you to run arbitrary commands when an inotify event happens on a
file or inside of a directory.

## Usage

```
autorun [--file <filename> | --dir <dirname>] <cmd>
```

For example:
```bash
autorun --file example.txt -- echo "An event occured on example.txt"
autorun --dir test -- echo "An event occured in test"
```

Any of the following commands will trigger the command `<cmd>`:
```bash
echo 10 > example.txt
mkdir test/d
touch test/d/f
echo 10 > test/d/f
```

## Installation

```bash
$ git clone https://github.com/CourrierGui/autorun
$ cd autorun
$ meson setup build
$ ninja -C build
# ninja -C build install
```
