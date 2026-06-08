# Companion dictionaries

Curated, device-compatible reference data the host can browse and push to the
LilyGO T-Embed. Parsed by `companion_dicts.py`; surfaced in the GUI
"Dictionaries" tab and via MCP (`device_dict_*`).

```
dictionaries/
  ir/      *.ir   Flipper-format IR signal files (protocol/address/command)
  rfid/    *.keys MIFARE key dictionaries (one 12-hex key per line, // comments)
  subghz/  *.sub  Flipper-format sub-GHz captures
```

## IR (`ir/*.ir`)
Flipper `.ir` format. Bruce protocol names: `NEC`, `NECext`, `RC5`, `RC6`,
`Samsung32`, `SIRC`/`SIRC15`/`SIRC20`, `Kaseikyo`. Bruce uses the first byte of
`address`/`command`, so spaced (`07 00 00 00`) and unspaced both work.
- **Send**: host issues `ir tx <protocol> <address> <command>` directly (no upload).
- **Deploy**: upload the `.ir` file to `/BruceIR/` for use from the device menu.
Drop more `.ir` files here (e.g. from the Flipper IRDB) and they appear automatically.

## RFID (`rfid/*.keys`)
MIFARE Classic key dictionaries. **Deploy** merges + uploads to
`/BruceRFID/keys.conf` (the device's key dictionary, read by the RFID menu).
The seeded list holds the public well-known default keys.

## Sub-GHz (`subghz/*.sub`)
Flipper `.sub` format (`Frequency:`, `Preset:`, `Protocol:`, `RAW_Data:`/`Key:`).
**Upload** to `/BruceRF/`, then `rf tx_from_file`. None are shipped by default —
transmitting sub-GHz may be regulated; add your own captures here.

> Legal note: only transmit on frequencies/with codes you are authorised to use.
