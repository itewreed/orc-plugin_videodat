# orc-plugin_videodat

[Decode-Orc](https://github.com/decode-orc/decode-orc) stage plugin that
decodes **Pro7 VideoDat** (1990–1994) from the vertical blanking interval.
It adds the sink stage **VideoDat Decoder** (`videodat_decoder`), built on
[orc-plugin_skeleton](https://github.com/decode-orc/orc-plugin_skeleton) and
modelled on decode-orc's Teletext Sink. decode-orc itself is not modified.

The decoder is a C++ port of `videodat_decoder.py` from
[channel-videodat-decoder](https://github.com/itewreed/channel-videodat-decoder)
(`src/decoder/`): line detection, H-sync reference, equalizer, bit slicing,
packet repair, permutation correction, file assembly, the service report, the
1990 variant and the decryption of services and paid software. It follows the
Python code step by step, including numpy's order of summation, and was
checked against it: on the test recordings and byte streams the logs, byte
streams and output files are identical. The stage's parameters map to the
decoder's command-line options; see [instructions.md](instructions.md), which
is also the stage help shown in decode-orc.

## Build (against a decode-orc checkout)

```bash
nix develop
cmake -S . -B build \
    -DORC_INTREE_SDK_DIR=/path/to/decode-orc \
    -DBUILD_TESTS=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

The plugin's toolchain tag must equal the host's, so build it in the same
environment as decode-orc (`nix develop` matches decode-orc's own flake).

The tests include a run of the whole stage on a synthetic VideoDat signal
that carries a file, which has to come out intact.

## Command-line decoder

`build/tools/videodat_decode` is the same decoder with the options of
`videodat_decoder.py`, for TBC files (`.tbc`, `.tbcy`) and byte streams
(`--bytes`); bt8x8 VBI captures are not supported. It is meant for comparing
the port with the Python decoder:

```bash
build/tools/videodat_decode tape.tbcy -o out_cpp --save-bytes out_cpp.bin
python3 videodat_decoder.py tape.tbcy -o out_py --save-bytes out_py.bin
diff -r out_py out_cpp
```

## Try it without installing

```bash
export ORC_STAGE_PLUGIN_PATHS=$PWD/build/lib/orc-stage-plugins
orc-cli --source tbc_source=input_path=tape.tbc \
        --sink "videodat_decoder=output_path=out:save_bytes=true"
```

or start `orc-gui` with the same environment variable and add the stage from
the Sink menu.

## Build for the decode-orc Flatpak

The Flatpak (io.github.decode_orc.decode-orc, v2.11.1, KDE runtime 6.9) is
built with gcc 14, so the plugin must be too (toolchain tag
`gcc14/libstdc++`). Build against the SDK headers of the installed release
with nixpkgs' gcc 14:

```bash
mkdir -p /tmp/orc-v2.11.1
git -C /path/to/decode-orc archive v2.11.1 orc/sdk | tar -x -C /tmp/orc-v2.11.1
GCC14=$(nix build --no-link --print-out-paths --inputs-from . nixpkgs#gcc14)
nix develop --command bash -c "CXX=$GCC14/bin/g++ CC=$GCC14/bin/gcc \
    cmake -S . -B build-flatpak -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DORC_INTREE_SDK_DIR=/tmp/orc-v2.11.1 \
    && cmake --build build-flatpak"
ORC_STAGE_PLUGIN_PATHS=$PWD/build-flatpak/lib/orc-stage-plugins \
    flatpak run io.github.decode_orc.decode-orc
```

Output directories must be inside the home directory, which is all the
Flatpak can write to.

## Limits

- decode-orc's path parameters only offer a file dialog, so the output
  directory is typed into it.
- A plugin is not told the input file name; the default byte-stream file is
  named after the output directory.
- numpy's `arctan2` (SIMD) can differ from the C library's `atan2` in the last
  bit, so on very rare lines a bit right at the decision threshold may come
  out differently than in the Python decoder. No such case showed up in the
  test recordings.

## Release

Packaging and the release manifest work as in the skeleton
(`scripts/package_local.sh`, `scripts/merge_manifests.sh`); assets are named
`orc-plugin_videodat_decoder_<platform>_abi<N>.<ext>`.
