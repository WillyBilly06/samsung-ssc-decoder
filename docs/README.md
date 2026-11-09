# Documentation

| Page | What it covers |
|---|---|
| [bitstream.md](bitstream.md) | **SSC frame format**: header, bitrates and frame sizes, symbol order, differences from Opus CELT |
| [a2dp.md](a2dp.md) | **SSC over Bluetooth**: codec element, flags, what to advertise, phone rules, packets, bitrate adaptation |
| [integration.md](integration.md) | **Use it in your project**: ESP-IDF component, decode loop, feeding it from an A2DP sink |
| [esp32-sink.md](esp32-sink.md) | **ESP32 firmware**: how SSC is plugged in, memory, CPU, console output |

## Research notes

Detailed notes from reverse-engineering Samsung's encoder and Bluetooth stack. They are kept for reference: they
explain *why* the decoder does what it does, with addresses in Samsung's libraries.

| # | Note | Topic |
|---|---|---|
| 1 | [Range coder and energy](notes/01-range-coder-and-energy.md) | Range coder, Laplace coding, coarse / fine / final energy |
| 2 | [Bit allocation](notes/02-bit-allocation.md) | `compute_allocation`, caps, allocation tables, offsets |
| 3 | [Band shapes (PVQ)](notes/03-band-shapes-pvq.md) | `quant_all_bands`, stereo, split bands, CWRS |
| 4 | [Signal path](notes/04-signal-path.md) | MDCT, window, post-filter, synthesis |
| 5 | [Frame flow](notes/05-frame-flow.md) | `ssc_encode` step by step, header, frame size |
| 6 | [A2DP stack](notes/06-a2dp-stack.md) | Samsung's Bluetooth stack: negotiation, packets, bitrate tables |
