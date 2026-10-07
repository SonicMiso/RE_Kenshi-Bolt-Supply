# RE_Kenshi Bolt Supply

Automatic crossbow ammunition resupply for Kenshi using RE_Kenshi/KenshiLib.

## v0.1

- Always enabled. No configuration or toggle.
- Checks player characters using a crossbow as their current ranged weapon.
- When their ammo count falls below 5, searches within 100m.
- Eligible sources:
  - friendly containers/buildings with an inventory;
  - other player squad characters.
- The character physically travels to the source before the transfer happens.
- A donor character must retain at least 5 ammo after the transfer.
- A donor already performing a supply job is not selected.
- A source is reserved while a supply job is active, preventing two shooters from selecting each other and creating a supply loop.
- The transfer uses the game's Inventory API only after the receiver reaches the source.

v0.1 treats Kenshi's `ITEM_AMMO` inventory function as crossbow ammunition. Ammo-type-specific matching will be tightened in a later version once the crossbow/GunClass ammo definition is exposed cleanly by the installed KenshiLib headers.

## Build

The GitHub Actions workflow builds against the public KenshiLib dependency package used by RE_Kenshi plugin projects.
