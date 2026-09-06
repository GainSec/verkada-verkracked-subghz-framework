# Provenance and claim status

Use these labels consistently:

- **observed**: recorded directly from an authorized system or signal.
- **statically recovered**: derived by inspecting software or data without
  executing it.
- **dynamically observed**: recorded while an authorized system executed.
- **analytically modeled**: produced from a mathematical/software model.
- **independently reconstructed**: reimplemented from observed behavior or
  factual interface descriptions without copied vendor code.
- **behaviorally reproduced**: repeated by the independent implementation.
- **physically validated**: confirmed on physical hardware with recorded gates.
- **operator-supplied**: provided at runtime and not shipped here.
- **hypothesized**: plausible but not confirmed.
- **unknown**: evidence is insufficient.

## Public claim matrix

| Behavior | Status | Public boundary |
|---|---|---|
| Radio-frame length/FCS parsing | Independently reconstructed; behaviorally reproduced | Portable tests pass; public physical capture not included |
| VMAC addressing and round-trip encoding | Independently reconstructed; behaviorally reproduced | Synthetic/reconstructed vectors only |
| VCMP structural parsing and CRC | Independently reconstructed; behaviorally reproduced | Encrypted bodies are structural; no live key is supplied |
| Typed scan/counter/join parsing | Statically recovered; independently reconstructed | Unknown fields remain opaque |
| Session/counter state model | Independently reconstructed | Model behavior is not physical confirmation |
| DSSS dictionary | Independently reconstructed; analytically modeled | Not claimed waveform-exact |
| OQPSK waveform fixture | Analytically modeled; independently generated | `hardware_exact=false`; not physically validated |
| Acquisition/demodulation | Behaviorally reproduced on synthetic/impaired inputs | Live BH-series RF validation not claimed here |
| SigMF import/evidence writing | Independently authored; behaviorally reproduced | File tests only; operator captures remain private |
| HackRF RX transport | Independently authored; behaviorally reproduced with test transport | Native hardware test depends on local libhackrf/device; CLI selection incomplete |
| EFR32 parser/peer model | Independently reconstructed | Not cycle-accurate; vulnerability-specific behavior excluded |
| i.MX7/EFR32 processor relationship | Statically recovered | Exact physical transport mapping unknown |
| BH61 end-to-end RF interoperability | Unknown in this public tree | Requires owner-supplied physical validation evidence |
| BH31 compatibility | Hypothesized / expected but unverified | Do not infer from BH61 modeling |

Implementation is evidence of software behavior, not evidence that a physical
device exhibited or accepted the same behavior. New evidence must preserve its
original provenance label rather than being upgraded by inference.
