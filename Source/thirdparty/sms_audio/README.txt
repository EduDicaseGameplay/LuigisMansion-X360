JAudio DSP microcode stand-in (mixer) from the Super Mario Sunshine Xbox 360 port (sms-xenon-languages,
platform/audio/dsp_mixer.{h,cpp}, PROTOCOL.md), MIT License (LICENSE.txt, Copyright (c) 2026 SoftwareAura LLC).
Taken from the local knowledge base copy (Projetos para Base de Conhecimento/Projetos/sms-xenon-languages).

Why it fits Luigi's Mansion: the voice parameter block (0x180 bytes x 64), the buses and the source types match
what the LM PC port documents for LM's JAudio microcode (Reference/LuigisMansionPC/docs/AUDIO.md, "Voice parameter
block"); on the Xbox 360 the host is big-endian like the GameCube, so the mixer reads LM's blocks in guest memory
as they are.

LM360 changes (marked "LM360" in dsp_mixer.cpp):
- direct-PCM stream voices (source 0x21): their ring address is a guest address, resolved through the main-memory
  resolver (Sunshine stores a native pointer there);
- the master level's fixed point defaults to 4.12 (shift 12): LM's sync-frame level is 4.12 (LM PC port
  AUDIO.md, "Frame protocol": DsetMixerLevel stores f*4096, LM uses 0x2000); Sunshine measured Q15 for SMS.
- the mixers' volume fixed point defaults to 16 (LM: (volume * sample) >> 16, "Voice pipeline" > Mixers); SMS's
  Q13 + Q15 and LM's Q16 + 4.12 give the same overall gain, LM's split keeps the buses at LM's scale;
- the output adds the centre, chorus, rear-right and surround buses as LM's microcode does (AUDIO.md, "Buses").
