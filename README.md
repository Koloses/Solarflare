<div align="center">
  <img src="sunshine.png" width="128" alt="Solarflare icon"/>
  <h1 align="center">Solarflare</h1>
  <h4 align="center">Self-hosted game stream host with the PyroWave ultra-low-latency codec.</h4>
</div>

## About

Solarflare is a fork of [Sunshine](https://github.com/LizardByte/Sunshine), the self-hosted game
stream host for Moonlight, extended with support for **PyroWave** — a GPU-only intra-frame wavelet
video codec that runs entirely in Vulkan compute. PyroWave trades the inter-frame compression of
H.264/HEVC/AV1 for extremely low, fixed encode latency and total independence from hardware video
encoders, which makes it a strong fit for LAN game streaming and for devices whose encoder blocks
are weak, broken, or missing entirely (the AMD BC-250 being the canonical example).

Everything Sunshine does is still here: hardware encoding on AMD, Intel, and Nvidia GPUs, software
encoding, a web UI for configuration and pairing, and compatibility with standard Moonlight
clients. PyroWave is an additional encoder you can select or force, negotiated only with clients
that support it — the [Aurora PC client](https://github.com/Koloses/aurora-qt) and
[Aurora for Android](https://github.com/Koloses/aurora-android).

## What the fork adds

- **PyroWave encoder** (vendored from the [WiVRn](https://github.com/WiVRn/WiVRn) fork of
  [Hans-Kristian Arntzen's PyroWave](https://github.com/Themaister/pyrowave)): Vulkan compute
  encode with RDO rate control, zero-copy DRM-PRIME capture import on Linux/KMS, and GPU color
  conversion.
- **Conditional replenishment**: unchanged 32x32 blocks are skipped on the wire and refreshed on a
  rolling schedule, cutting bitrate drastically on mostly-static content; the freed budget is
  spent on image quality.
- **Loss resilience**: RTP-payload-aligned packetization, self-recovering client-side parsing, and
  refresh requests answered with full frames.
- **Adaptive FEC and adaptive bitrate** (client opt-in), with capped overhead.
- **HDR10 (BT.2020 + PQ) and 4:4:4 chroma** PyroWave modes.
- **Phase-locked frame pacing** cooperating with client present-wait feedback.
- **Web UI additions**: a dedicated PyroWave tab under Configuration, PyroWave in the forceable
  encoder list, and encoder-aware Advanced settings.
- **Rebranding**: Solarflare name and artwork throughout; config lives in
  `~/.config/solarflare` (`solarflare.conf`, `solarflare_state.json`, `solarflare.log`);
  upstream update checks and donation prompts removed.

## Building

Build exactly like upstream Sunshine (see the
[Sunshine docs](https://docs.lizardbyte.dev/projects/sunshine)), with PyroWave enabled via:

```bash
cmake -DSUNSHINE_ENABLE_PYROWAVE=ON ...
```

A Vulkan SDK is required. Shader generation needs `python3` and `glslangValidator` on the PATH.

## Upstream and credits

- [Sunshine](https://github.com/LizardByte/Sunshine) by LizardByte — the project this fork is
  based on; all host, capture, and streaming infrastructure comes from there.
- [PyroWave](https://github.com/Themaister/pyrowave) by Hans-Kristian Arntzen — the codec design
  and reference implementation.
- [WiVRn](https://github.com/WiVRn/WiVRn) — the PyroWave fork vendored here, plus the original
  encoder integration this port follows.
- [Moonlight](https://moonlight-stream.org) — the streaming protocol and client ecosystem.

## License

GPL-3.0, same as upstream Sunshine. Original copyright and attribution notices are retained.
