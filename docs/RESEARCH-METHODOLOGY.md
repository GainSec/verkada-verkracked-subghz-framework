# Reproducible research methodology

## Evidence flow

1. State the question and current provenance level.
2. Prefer passive observation and synthetic tests before powered hardware work.
3. Preserve immutable raw input outside Git and record its SHA-256, tool
   versions, settings, UTC window, and authorization context.
4. Analyze a copy. Keep decoding, interpretation, and physical-validation
   claims separate.
5. Reconstruct the smallest protocol/model behavior in independent source.
6. Create a synthetic regression vector when it preserves the technical
   property under test.
7. Compare model output with observed data byte-by-byte or sample-by-sample;
   record mismatches and negative results.
8. Upgrade a claim only when the evidence definition in `PROVENANCE.md` is met.

## Passive RF acquisition

Use a receive-only SDR on an isolated or shielded bench where practical.
Record receiver identity privately, sample format/rate, center frequency, gain,
clock source, antenna/attenuation, discontinuities, and clipping. A spectral
peak or periodic energy is not protocol confirmation. Require the modeled
acquisition boundary, PHR, DSSS, FCS, VMAC, and VCMP checks before calling a
burst decoder-valid.

Sanitize or regenerate public vectors. A synthetic replacement must remain
labeled synthetic even if it exercises the same parser path.

## Static and controlled dynamic analysis

Static recovery may support field sizes, constants, dispatch relationships, or
state hypotheses, but it does not prove runtime behavior. Controlled dynamic
observation on owner-controlled hardware must record exact inputs, outputs,
versions, and stop conditions. Do not publish vendor code, live identifiers,
credentials, or embargoed security context.

## Physical validation

Physical validation requires an owned target, calibrated/appropriate equipment,
receive-only or otherwise explicitly reviewed test boundaries, raw artifact
hashes, and repeatable positive results. This public release includes no claim
that the analytical waveform is hardware-exact and no claim that BH31 has been
physically validated.
