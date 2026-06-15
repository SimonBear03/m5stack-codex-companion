# Cardputer References

This repo now has a Cardputer ADV PlatformIO target alongside the original StickS3 target. The first Cardputer pass is display-only parity with Cardputer-native keyboard navigation; it is not yet a Codex text-input device.

Useful references found during planning:

- M5Cardputer Arduino library: https://github.com/m5stack/M5Cardputer
- M5Stack Cardputer keyboard docs: https://docs.m5stack.com/en/arduino/m5cardputer/keyboard
- M5Cardputer UserDemo: https://github.com/m5stack/M5Cardputer-UserDemo
- M5Launcher: https://github.com/bmorcelli/Launcher
- MicroHydra: https://github.com/Lana-chan/Cardputer-MicroHydra/
- GitHub topic: https://github.com/topics/m5stack-cardputer

Design takeaways for this firmware:

- Keep navigation predictable and keyboard-first on Cardputer.
- Favor short, stable pages over dense scrolling content.
- Treat status, approvals, plan, and limits as separate modes.
- Keep payloads compact because BLE UART and tiny displays do not benefit from full transcript text.

Current Cardputer target notes:

- Build with `pio run -e cardputer_adv`.
- Advertises as `Codex-CP-XXXX`.
- Defaults to fixed landscape; IMU autorotation is skipped.
- Dashboard controls: Up/Down move one line older/newer, Left/Right move one page older/newer, Enter opens settings, Backspace/Esc and short GO/G0 jump to newest, and long GO/G0 enters display sleep. The arrow and Esc physical keys also work without Fn via their base characters: `;`, `.`, `,`, `/`, and `` ` ``.
- Settings controls: Up/Down move selection, Left/Right change values backward/forward, Enter or short GO/G0 changes values forward, Backspace/Esc closes settings, and long GO/G0 enters display sleep. The no-Fn base-character aliases work in settings too.
- Battery telemetry is best-effort until validated on physical Cardputer ADV hardware.
