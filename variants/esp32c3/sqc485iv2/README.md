# SQC485Iv2 variant (ESP32-C3 + SX1262)

This variant is for the **SQC485Iv2** board, which uses the **Heltec HT-CT62** module (ESP32-C3 + SX1262).

Current status:
- Pin mapping matches the HT-CT62 reference design (`GPIO9` user key, `GPIO2` LED power/state, SX1262 pins).
- Battery ADC measurement is **not** implemented/available on this revision.

Notes:
- We keep `custom_meshtastic_hw_model` compatible for now (until upstream assigns a dedicated HW model id).
