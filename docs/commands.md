# BreezyBox Cardputer Commands

### File Operations
```
ls [path]           - List directory contents
cat <file>          - Display file contents
head [-n N] <file>  - Show first N lines (default 10)
tail [-n N] <file>  - Show last N lines (default 10)
more <file>         - Paginate file contents
wc [-lwc] <file>    - Count lines/words/chars
cp <src> <dst>      - Copy file
mv <src> <dst>      - Move/rename file
rm [-r] <file...>   - Remove file or directory
mkdir <dir>         - Create directory
```

### Navigation
```
cd [path]           - Change directory
pwd                 - Print working directory
```

### System
```
free                - Show memory usage (SRAM/PSRAM)
df                  - Show filesystem space
du [-s] [path]      - Show disk usage
date [datetime]     - Show/set date and time
clear               - Clear screen
sh <script>         - Run shell script
help                - List all commands
```

### Networking
```
wifi scan                    - Scan for WiFi networks
wifi connect <ssid> [pass]   - Connect to WiFi
wifi disconnect              - Disconnect from WiFi
wifi status                  - Show connection status
wifi forget                  - Forget saved network
httpd [dir] [-p port]        - Start HTTP file server
```

### Programs
```
eget <user/repo>    - Download ELF from GitHub releases
app_name            - run app_name ELF file from /root/bin/ or CWD
```

### Built-in
```
echo [text...]      - Print text to stdout
```

## I/O Redirection

```bash
$ echo "Hello" > /root/test.txt    # Write to file
$ echo "World" >> /root/test.txt   # Append to file
$ cat < /root/test.txt             # Read from file
$ ls | head                        # Pipe output
```

## Chaining
```bash
$ echo "Hello" && echo "World"
$ echo "Hello" ; echo "World"
```

## Virtual Terminals

Switch between terminals using:
- **F1-F4**: Switch to VT0-VT3
- **Ctrl+F1-F4**: Switch to VT0-VT3 (alternative)

## Built-in apps
* `vi`, 
* `plasma`
* `termbench`
* `wget`
* `gzip`
* `gunzip`
* `help`
* `ping`
* `ssh`, `sshcfg`

## Test utilities

* `testgfx`
* `colortest`
* `keytest`


## Extra shell utilities:
  - `touch`, `chmod`, `ln`
  - `find`, `grep`, `sed`, `sort`, `uniq`, `cut`, `tr`
  - `which`, `type`
  - `ps`, `kill`
  - `uname`
  - `env`, `export`, `unset`
  - `history`
  - `source`, `.`
  - `true`, `false`, `test`, `[`
  - `sync`
  - `sleep`
