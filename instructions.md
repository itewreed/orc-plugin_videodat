# VideoDat Decoder

Decodes **Pro7 VideoDat** (1990–1994), a data service broadcast in the
vertical blanking interval of the German channel Pro7, and writes the received
files, the service report (clock, schedule, bit lists, encrypted services) and
the decoder log into an output directory.

Both transmission variants are supported and detected automatically:

| Variant | Bit rate | Bytes per line |
|---|---|---|
| 1993 | 2 Mbit/s | 10, scrambled packets with 16-bit checksum |
| 1990 | 800 kbit/s | 4, SOH packets, several channels on separate lines |

## How it works

The stage is a sink: nothing happens until you **trigger** it. It then

1. copies the first 40 lines of every field of its input (composite, or the
   luma of a Y/C source) into a temporary file `.videodat_vbi.tbc` in the
   output directory,
2. runs `videodat_decoder.py` from the
   [channel-videodat-decoder](https://github.com/itewreed/channel-videodat-decoder)
   project on it, and
3. deletes the temporary file. The decoder's console output is saved as
   `videodat_decoder.log`; the trigger status shows its summary.

The temporary file needs about 4.5 MB per second of recording (16 GB per
hour) on the drive of the output directory while the decoder runs.

## Requirements

Python 3 with `numpy` (and `Pillow` for the bit-list images). Set
**Python Interpreter** to an interpreter that has them, for example the
`python` of a virtual environment. **Decoder Script** points at
`videodat_decoder.py`; left empty, the stage uses the copy installed in
`videodat-decoder/` next to the plugin library.

## Parameters

| Parameter | Decoder option | Meaning |
|---|---|---|
| Output Directory | `-o` | Created if missing. The browse button opens a file dialog: go to the parent directory and type the directory name |
| Variant | `--variant` | Automatic, 1993 or 1990 |
| Lines | `--lines` | Frame lines carrying VideoDat, e.g. `11,12,13,324,325,326`; empty = automatic. Only 1–40 and 314–353 are passed to the decoder |
| Scan Seconds | `--scan-seconds` | How far into the recording the line search may look |
| No H-Sync Reference | `--no-sync` | Do not use the H-sync as timing reference |
| No Equalizer | `--no-eq` | 1993 only |
| Write Incomplete Files | `--partial` | Missing blocks become zero bytes, name ends in `.unvollstaendig` |
| Skip Other Services | `--keine-dienste` | Files only |
| Dump Other Packets | `--dump-other` | 1993 only, raw packets to `andere_dienste/` |
| Decrypt Services | `--decrypt-services` | 1993 only |
| Decrypt Files | `--decrypt-files` | 1993 only, paid software to `decrypted_files/` |
| Save Byte Stream | `--save-bytes` | Byte stream for later runs with `--bytes` |
| Byte Stream File | | Empty = `<output directory>/<directory name>.bytes` |

Options that exist only for the 1993 format are greyed out while Variant is
set to 1990. With Variant on Automatic they stay available; the decoder
ignores them if it detects a 1990 recording.

Not offered, because they make no sense for decode-orc input: `--format`,
`--bytes`, `--tbc-width`, `--tbc-field-lines` (set by the stage), and the
bt8x8 capture options `--line-length`, `--lines-per-frame`,
`--no-line-tracking`.

## Output directory

```
strom_XX/        received files per data stream (1993)
kanal_XX/        received files per channel (1990)
dienste/         service report dienste.txt, bit lists, raw service data
videodat_decoder.log
```
