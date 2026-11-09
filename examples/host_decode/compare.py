"""Compare decoded PCM against the original input.

Usage: python -I compare.py original.s16le decoded.s16le channels
Finds the codec delay by cross-correlation, then prints the SNR per channel.
"""
import struct
import sys

import numpy as np


def load(path, ch):
    data = np.frombuffer(open(path, 'rb').read(), dtype='<i2').astype(np.float64)
    return data[: len(data) // ch * ch].reshape(-1, ch)


def main():
    orig_path, dec_path, ch = sys.argv[1], sys.argv[2], int(sys.argv[3])
    a, b = load(orig_path, ch), load(dec_path, ch)
    n = min(len(a), len(b))
    # Delay search on channel 0 over the first few seconds.
    seg = min(n, 44100 * 2)
    best_d, best_c = 0, -1e30
    for d in range(0, 2000):
        if d + seg > len(b):
            break
        c = float(np.dot(a[:seg - 2000, 0], b[d:d + seg - 2000, 0]))
        if c > best_c:
            best_c, best_d = c, d
    d = best_d
    m = min(len(a), len(b) - d)
    skip = 4096  # ignore the start-up transient
    for c in range(ch):
        x, y = a[skip:m, c], b[d + skip:d + m, c]
        err = x - y
        snr = 10 * np.log10(np.sum(x * x) / max(np.sum(err * err), 1e-9))
        gain = np.dot(x, y) / max(np.dot(x, x), 1e-9)
        print(f'channel {c}: delay {d} samples, SNR {snr:.2f} dB, gain {gain:.4f}')


if __name__ == '__main__':
    main()
