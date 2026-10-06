---
name: Bug report
about: Report a reproducible driver, build or overlay failure
---

Board (Zero W or Zero 2 W), TagTagTag revision, power supply:
Driver revision/tag and toolchain version:
Kernel release (`uname -a`), architecture and header package version:
Overlay/config changes and competing PWM/audio drivers:
Root writable or genuinely read-only:

Expected behavior:
Observed behavior:
Minimal commands to reproduce (including the final `sync` result):

Attach relevant boot/driver kernel logs, build/modpost output, LED `multi_index`,
`max_brightness`, brightness/intensity values and timing configuration.
For waveform faults, attach a scope/logic-analyzer capture of GPIO13 showing
bit timing, word boundaries and reset; distinguish measurements from inference.
Collect volatile logs before stopping/rebooting. Remove credentials/private data.
