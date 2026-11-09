# Decode SSC files on a PC

`ssc_dec_tool` decodes an `.ssc` file to raw 16-bit stereo PCM. `compare.py` measures the quality against the
original signal.

## Build

From the repository root:

```bash
cmake -B build
cmake --build build
```

Or without CMake:

```bash
cc -O2 -DSSC_DEBUG_TELL -Issc_decoder ssc_decoder/*.c examples/host_decode/ssc_dec_tool.c -lm -o ssc_dec_tool
```

## Decode

```bash
build/ssc_dec_tool test_vectors/ssc/mix_192000.ssc out.s16le 44100
ffplay -f s16le -ar 44100 -ac 2 out.s16le
```

Arguments: input file, output file, and the codec sample rate (`44100`, `48000` or `96000`, default `44100`).
The tool prints the number of frames, errors and size mismatches. Set `SSC_TELL=1` in the environment to print
each frame's final bit position.

Input format: a sequence of `[u16 little-endian length][SSC frame]`, as in [`test_vectors/ssc`](../../test_vectors/ssc).

## Measure quality

```bash
pip install numpy
python compare.py ../../test_vectors/input/mix.s16le out.s16le 2
```

The script finds the codec delay by cross-correlation and prints the SNR per channel.
