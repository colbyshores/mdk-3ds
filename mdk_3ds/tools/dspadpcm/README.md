# gc-dspadpcm-encode (vendored)

Nintendo GameCube/Wii DSP-ADPCM encoder, vendored unchanged from
<https://github.com/jackoalan/gc-dspadpcm-encode> by way of
`../t2k/tools/vendor/gc-dspadpcm-encode` (MIT License, Copyright (c) 2015 Jack
Andersen -- see `LICENSE`, which must stay with these sources).

`mdk_3ds/tools/music_dsp_convert.py` builds it (`cc -O2 main.c grok.c -lm`)
into its work directory and runs it once per track: `dspadpcm <in.wav> <out.dsp>`,
input PCM16 mono WAV. It also writes `<out.dsp>.wav` (its decode); the converter
deletes that.

Header note: this tool writes `num_nibbles` (offset 4) counting the frame
headers too (16 per frame). The 3DS player sizes the stream from the header's
`num_samples` (offset 0) and the file length, never from `num_nibbles`.
