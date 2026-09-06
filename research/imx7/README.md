# i.MX7 / EFR32 interface boundary

BH61 research indicates a split architecture: an i.MX7 application processor
hosts higher-level software while an EFR32-family controller handles the
Sub-GHz radio path. The public SDR implementation models the RF and protocol
side of that boundary; it does not emulate the i.MX7 operating environment.

The exact physical host-controller transport mapping remains unverified in the
public project. A bidirectional, checksum-valid passive logic capture would be
needed before assigning pins, ports, rates, or flow-control semantics as
physically validated.

This directory intentionally contains no extracted device tree, kernel
configuration, bootloader environment, root-filesystem file, service binary,
shell transcript, device credential, or debug-access procedure. Users may
compare the public protocol model with material obtained from hardware they are
authorized to analyze, but those inputs must remain outside the repository.

Evidence status: the processor/controller relationship is **statically
recovered**; the exact transport and board-level mapping are **hypothesized or
unknown** pending passive physical validation.
