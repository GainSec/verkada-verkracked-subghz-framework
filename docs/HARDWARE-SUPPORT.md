# Hardware and backend support

The primary research target is the [BH61 Wireless Alarm Hub][bh61-guide].
Verkada's [Classic Alarms wireless-device guide][wireless-setup] documents the
BH61 ecosystem and its regional 915 MHz (US/Canada) and 868 MHz (UK/EU)
operation. The repository implements independently authored passive-analysis
and protocol-modeling code; the vendor links below describe the commercial
hardware, not validation performed by this project.

| Target | Code-path support | Model/capture evidence | Status |
|---|---|---|---|
| [BH61 family][bh61-guide] | Frame, protocol, DSP, and EFR32 model | Static/reconstructed basis; no public live capture | Modeled; public physical validation not claimed |
| BH31 family | Shared parser may apply | No BH31-specific public capture or physical test | Expected but unverified |
| HackRF One/Pro | Enumeration, configuration, signed-IQ conversion, bounded RX queue, libhackrf adapter | Unit/fake-transport tests; native availability depends on host | RX library implemented; CLI capture selection incomplete |
| File / raw `cf32_le` | Decode, scan, analytical generation, capture normalization | Portable tests | Supported and behaviorally reproduced |
| SigMF `cf32_le` | Metadata/data input and passive output | Portable tests | Supported |
| UHD/USRP | None | None | Unsupported |
| RTL-SDR | None | None | Unsupported |
| SDR transmission | No native implementation or CLI command | Offline file-device tests only | Unsupported over the air |

The vendor-documented BH61 peripheral family consists of the [BR31 door
sensor][br31-guide], [BR32 motion sensor][br32-guide], [BR33 panic
button][br33-guide], [BR34 glass-break sensor][br34-guide], [BR35 water-leak
sensor][br35-guide], and [BX21 wireless relay][bx21-guide]. Their inclusion here
provides ecosystem context and does not claim individual physical validation.

Offline fixture, raw `cf32_le`, and SigMF analysis requires no SDR. Passive
live-RF work requires a HackRF One or HackRF Pro, libhackrf development files,
a data-capable USB connection, and a receive antenna appropriate for the
operator's regional band. Direct HackRF-to-`capture` CLI selection is not yet
implemented, so the supported end-to-end CLI path currently starts with a raw
`cf32_le` or SigMF file supplied by the operator.

Hardware integration tests require the operator's own receiver, BH hardware,
RF-safe setup, and lawful regional configuration. Their absence is not a unit
test failure and must be reported separately.

[bh61-guide]: https://docs.verkada.com/docs/wireless-alarm-hub-install-guide.pdf
[wireless-setup]: https://help.verkada.com/classic-alarms/installation/alarm-setup-and-install-best-practices/set-up-your-wireless-alarm-devices
[br31-guide]: https://docs.verkada.com/docs/br31-quick-start-guide.pdf
[br32-guide]: https://docs.verkada.com/docs/br32-quick-start-guide.pdf
[br33-guide]: https://docs.verkada.com/docs/br33-quick-start-guide.pdf
[br34-guide]: https://docs.verkada.com/docs/br34-quick-start-guide.pdf
[br35-guide]: https://docs.verkada.com/docs/br35-quick-start-guide.pdf
[bx21-guide]: https://docs.verkada.com/docs/wireless-relay-bx21-datasheet.pdf
