# Cardputer References

This repo still targets StickS3 first. Cardputer projects are useful references for small M5Stack screen interaction patterns, not a hardware target change.

Useful references found during planning:

- M5Cardputer Arduino library: https://github.com/m5stack/M5Cardputer
- M5Stack Cardputer keyboard docs: https://docs.m5stack.com/en/arduino/m5cardputer/keyboard
- M5Cardputer UserDemo: https://github.com/m5stack/M5Cardputer-UserDemo
- M5Launcher: https://github.com/bmorcelli/Launcher
- MicroHydra: https://github.com/Lana-chan/Cardputer-MicroHydra/
- GitHub topic: https://github.com/topics/m5stack-cardputer

Design takeaways for this firmware:

- Keep navigation predictable and button-first.
- Favor short, stable pages over dense scrolling content.
- Treat status, approvals, plan, and limits as separate modes.
- Keep payloads compact because BLE UART and tiny displays do not benefit from full transcript text.
