# WiFi Vendor DB

`Presence Monitor` can resolve vendors from a full IEEE OUI database.

Expected files (auto-detected):
- `/Bruce/wifi/oui.csv`
- `/Bruce/oui.csv`
- `/wifi/oui.csv`
- `/oui.csv`

Format:
- One entry per line: `AABBCC,Vendor Name`

To regenerate the latest database from IEEE registries:

```bash
python scripts/fetch_oui_db.py
```

This writes `sd_files/wifi/oui.csv`.
