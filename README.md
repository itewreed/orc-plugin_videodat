# orc-plugin_videodat

[Decode-Orc](https://github.com/decode-orc/decode-orc) stage plugin that
decodes **Pro7 VideoDat** (1990–1994) from the vertical blanking interval.
It adds the sink stage **VideoDat Decoder** (`videodat_decoder`), built on
[orc-plugin_skeleton](https://github.com/decode-orc/orc-plugin_skeleton) and
modelled on decode-orc's Teletext Sink. decode-orc itself is not modified.

The decoding is done by `videodat_decoder.py` from
[channel-videodat-decoder](https://github.com/itewreed/channel-videodat-decoder).
On trigger the stage writes the first 40 lines of every field of its input
(CVBS, or the luma of a Y/C source) as a reduced TBC file, runs the decoder
on it and removes the file again. The stage's parameters map to the decoder's
command-line options; see [instructions.md](instructions.md), which is also
the stage help shown in decode-orc.

Requirements at run time: Python 3 with `numpy` (and `Pillow`).

## Build (against a decode-orc checkout)

```bash
nix develop
cmake -S . -B build \
    -DORC_INTREE_SDK_DIR=/path/to/decode-orc \
    -DVIDEODAT_DECODER_DIR=/path/to/channel-videodat-decoder \
    -DBUILD_TESTS=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

`VIDEODAT_DECODER_DIR` is optional. When set, the decoder's Python scripts are
copied to `videodat-decoder/` next to the plugin library, where the stage
finds them without the **Decoder Script** parameter.

The plugin's toolchain tag must equal the host's, so build it in the same
environment as decode-orc (`nix develop` matches decode-orc's own flake).

## Try it without installing

```bash
export ORC_STAGE_PLUGIN_PATHS=$PWD/build/lib/orc-stage-plugins
orc-cli --source tbc_source=input_path=tape.tbc \
        --sink "videodat_decoder=output_path=out:python=/path/to/venv/bin/python:save_bytes=true"
```

or start `orc-gui` with the same environment variable and add the stage from
the Sink menu.

## Limits

- Python is needed at run time, so a binary installed through decode-orc's
  plugin manager also needs the decoder scripts and a Python with numpy.
- decode-orc's path parameters only offer a file dialog, so the output
  directory is typed into it.
- A plugin is not told the input file name; the default byte-stream file is
  named after the output directory.
- The process runner has a Windows implementation that has not been tested.

## Release

Packaging and the release manifest work as in the skeleton
(`scripts/package_local.sh`, `scripts/merge_manifests.sh`); assets are named
`orc-plugin_videodat_decoder_<platform>_abi<N>.<ext>`.
