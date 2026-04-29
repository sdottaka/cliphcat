# cliphcat
`cliphcat` is a lightweight command-line tool for reading the Windows clipboard and clipboard history.
It is primarily designed for integration with WinMerge.

---

## Features
* Read current clipboard or history items
* Multiple format support: `text` (UTF-8) / `html` (fragment) / `rtf` / `png` (via WIC)
* URL-style access (`clip://`)
* Outputs to stdout or file
* Windows 7, 8, 10, 11 support

---

## Usage
```
cliphcat [options] [index|alias|url]
```

### Index
| Value           | Description           |
| --------------- | --------------------- |
| `1`, `latest`   | Current clipboard     |
| `2`, `previous` | Previous history item |
| `N`             | Nth item (1 = latest) |

---

## Examples
```
# Get latest text (default)
cliphcat

# Get HTML from previous item
cliphcat 2 -f html

# Save as PNG
cliphcat 1 -f png -o image.png

# Using URL scheme
cliphcat "clip://2?format=html"

# List clipboard history
cliphcat --list
```

---

## Options
| Option                | Description                                   |
| --------------------- | --------------------------------------------- |
| `-f, --format <type>` | Specify format (`text`, `html`, `rtf`, `png`) |
| `-o, --output <file>` | Output to file                                |
| `--raw`               | Output raw data                               |
| `-l, --list`          | List clipboard history                        |
| `-C, --clear`         | Clear clipboard history                       |

---

## URL Scheme
```
clip://<index>?format=<type>
clipboard://<index>?format=<type>
```

---

## Notes
* Clipboard history is only available on Windows 10 (version 1809) or later
* On Windows 7 and 8, only the current clipboard (index 1) is supported
  * Accessing history items (index 2 or higher) will result in an error
* HTML output returns the fragment only (no headers)
* PNG format uses Windows Imaging Component (WIC) for encoding

## Author
Takashi Sawanaka

---

## License
MIT License