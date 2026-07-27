# wxl-equip-extension

A [WarcraftXL](https://github.com/WarcraftXL/WarcraftXL) module that dramatically expands what `ItemDisplayInfo.dbc` can do. It extends M2 model attachment support to every equippable slot, adds a second model channel to all slots, and introduces collection M2 support with per-geoset filtering — all driven entirely by DBC data, with no client code changes per item.

---

## Requirements

- **WarcraftXL** installed in the WoW 3.3.5a client directory (`d3d9.dll` proxy + `WarcraftXL.dll`).
- This module is compiled into `WarcraftXL.dll` automatically — no separate DLL, no additional files needed at runtime.

---

## Building

This module is auto-globbed by the WarcraftXL CMake build. To include it, clone or copy the `scripts/wxl-equip-extension/` directory into your WarcraftXL source tree, then build normally:

```powershell
$env:PATH = "C:\path\to\cmake\bin;$env:PATH"
.\build.ps1
```

Deploy the resulting `WarcraftXL.dll` and `d3d9.dll` into your WoW client directory as usual.

---

## What changes in `ItemDisplayInfo.dbc`

### Slot coverage

Vanilla 3.3.5a only supports `ModelName_1`, `ModelName_2`, `ModelTexture_1`, and `ModelTexture_2` for Head and Shoulder slots. This module extends support to the slots listed below. Slots not listed (Neck, Rings, Trinkets, weapons) are deferred and not yet handled.

| Slot | Model1 | Model2 | Texture1 | Texture2 | Icon2 settings |
|------|--------|--------|----------|----------|----------------|
| Head | was partial | **added** | - | **added** | **added** |
| Shoulder | was partial | was partial | -  | - | **added** |
| Shirt | **new** | **new** | **new** | **new** | **new** |
| Chest | **new** | **new** | **new** | **new** | **new** |
| Waist | **new** | **new** | **new** | **new** | **new** |
| Legs | **new** | **new** | **new** | **new** | **new** |
| Feet | **new** | **new** | **new** | **new** | **new** |
| Wrist | **new** | **new** | **new** | **new** | **new** |
| Hands | **new** | **new** | **new** | **new** | **new** |
| Back | **new** | **new** | **new** | **new** | **new** |
| Tabard | **new** | **new** | **new** | **new** | **new** |

Each equipped item can now attach up to **two independent M2 models** to any character or NPC wearing that armor.

---

### `ModelName_1` and `ModelName_2` — geoset filtering and collections

The model name fields work as before for simple per-slot models. Two new capabilities are added:

#### Collection M2s

Append a colon and a comma-separated list of geoset IDs to activate a collection:

```
chestplate_paladin_tier2.mdx:1201,2301
```

- The `:` switches the model lookup path from the per-slot directory (`Item\ObjectComponent\<SlotName>\`) to the shared collections directory (`Item\ObjectComponent\collections\`). This matches the retail pathing, so retail-exported collection M2s can be used without renaming.
- Geoset IDs listed after `:` are the **only** geosets that will render. All others are hidden. Geoset `0` (always-on geometry) is never filtered out.
- Combine with **flag `0x40`** (new suffix format) if the collection uses the modern `Race_Gender` naming convention.

#### Standard (non-collection) models

No colon in the name: the lookup path is the normal per-slot directory, suffixed with race and gender as usual (controlled by flags in Icon2, see below).

---

### `ModelTexture_1` and `ModelTexture_2`

These columns work exactly as they did for Head and Shoulders. No format change — they are now honoured for all equippable slots.

---

### `Icon2` — attachment point and flag configuration

The `Icon2` field is unused in stock 3.3.5a and is repurposed to configure how Model1 and Model2 are loaded and attached.

**Format:** `<model1_attachment>:<model2_attachment>:<flags>`

All three parts are optional. Omit a value to keep the slot default. Examples:

| Icon2 value | Meaning |
|-------------|---------|
| *(empty)* | Use slot defaults for both attachment points and all flags |
| `11:55` | Model1 on attachment 11, Model2 on attachment 55 |
| `:34` | Model2 on attachment 34, Model1 uses slot default |
| `::6` | Flags = `0x4 \| 0x2` (no gender or race suffix), attachments at defaults |
| `19:19` | Both models on attachment 19 (Ground_Base) |
| `::96` | Flags = `0x20 \| 0x40` (subfolder + new suffix format), attachments at defaults |

#### Flags

| Flag | Name | Effect |
|------|------|--------|
| `0x1` | Hide built-in geosets | Skip the normal slot geoset rendering. Use when the M2 model contains all the geometry and the built-in slot geosets should not show. |
| `0x2` | No gender suffix | Do not append `_M` / `_F` to the model path. Use for gender-neutral models. Combine with `0x4` to strip both suffixes entirely (the separator `_` is also skipped). |
| `0x4` | No race suffix | Do not append the race code to the model path. Combine with `0x2` to strip both suffixes. |
| `0x8` | Append race to texture | Append the race code to the texture path (textures are normally not suffixed). |
| `0x10` | Append gender to texture | Append the gender code to the texture path. |
| `0x20` | Model in subfolder | Place the model in a subfolder named after the base model (without suffixes). For example, `chestplate_paladin_tier2_HuF.mdx` resolves to `Item\ObjectComponent\Chest\chestplate_paladin_tier2\chestplate_paladin_tier2_HuF.mdx`. Texture paths follow the same subfolder. Useful for packaging models for easy distribution and merging. |
| `0x40` | New suffix format | Use the retail naming convention with an extra underscore between race and gender: `Hu_F` instead of `HuF`. Only applies when the model or texture actually needs suffixing (i.e. `0x2`/`0x4` are not both set, or `0x8`/`0x10` are both set). Lets you use retail-exported models without renaming. |

---

## Default attachment points per slot

If no attachment is specified in `Icon2`, the following defaults apply:

| Slot | Model1 attachment | Model2 attachment |
|------|-------------------|-------------------|
| HEAD | 11 (Helmet) | 55 (HeadTop) |
| SHOULDER | 6 (Left Shoulder) | 5 (Right Shoulder) |
| SHIRT | 34 (Chest) | 34 (Chest) |
| CHEST | 34 (Chest) | 34 (Chest) |
| WAIST | 53 (BeltBuckle) | 53 (BeltBuckle) |
| LEGS | 9 (HipRight) | 10 (HipLeft) |
| FEET | 47 (LeftFoot) | 48 (RightFoot) |
| WRIST | 3 (ElbowRight) | 4 (ElbowLeft) |
| HANDS | 1 (HandRight) | 2 (HandLeft) |
| BACK | 12 (Back) | 12 (Back) |
| TABARD | 34 (Chest) | 34 (Chest) |

---

## Sample DBC rows — Judgement armor set

The examples below are a real two-colour Judgement armor set implemented with this module. Several pieces share a single collection M2 (`plate_raidpaladint2_d_01_chest.mdx`), using geoset filtering to show only the geometry relevant to each slot. Collection models live under `Item\ObjectComponent\collections\palat2\`. The belt uses a standalone per-slot model with the subfolder flag (`0x20`) so it can be packaged in its own directory.
This is mostly meant to show of the various ways to use it, not how it is best to be used.

### Judgement (original)

| ID | Slot | Model1 | Model2 | ModelTexture1 | ModelTexture2 | Icon1 | Icon2 |
|----|------|--------|--------|---------------|---------------|-------|-------|
| 45888 | Head | `palat2\plate_raidpaladint2_d_01_helm.mdx` | | `palat2\plate_raidpaladint2_d_01_helm_be_m_6037071` | | inv_plate_raidpaladint2_d_01_HELM | |
| 34258 | Shoulder | `palat2\plate_raidpaladint2_d_01_shoulder_l.mdx` | `palat2\plate_raidpaladint2_d_01_shoulder_r.mdx` | `palat2\plate_raidpaladint2_d_01_shoulder_l_6037139` | `palat2\plate_raidpaladint2_d_01_shoulder_l_6037139` | inv_plate_raidpaladint2_d_01_shoulder | |
| 33635 | Chest | `palat2\plate_raidpaladint2_d_01_chest.mdx:2201` | | `palat2\plate_raidpaladint2_d_01_chest_be_m_6037063` | | inv_plate_raidpaladint2_d_01_chest | `19` |
| 33633 | Belt | `plate_raidpaladint2_d_01_belt.mdx` | | `plate_raidpaladint2_d_01_belt_6037176` | | inv_plate_raidpaladint2_d_01_belt | `53::38` |
| 33634 | Bracers | | | | | inv_plate_raidpaladint2_d_01_bracer | |
| 33636 | Gloves | `palat2\plate_raidpaladint2_d_01_chest.mdx:401` | `palat2\plate_raidpaladint2_d_01_chest.mdx:2301` | `palat2\plate_raidpaladint2_d_01_chest_be_m_6037063` | `palat2\plate_raidpaladint2_d_01_chest_be_m_6037063` | inv_plate_raidpaladint2_d_01_glove | `19:19` |
| 33637 | Legs | `palat2\plate_raidpaladint2_d_01_chest.mdx:1301` | | `palat2\plate_raidpaladint2_d_01_chest_be_m_6037063` | | inv_plate_raidpaladint2_d_01_pant | `19` |
| 33639 | Boots | `palat2\plate_raidpaladint2_d_01_chest.mdx:2001` | | `palat2\plate_raidpaladint2_d_01_chest_be_m_6037063` | | inv_plate_raidpaladint2_d_01_boot | `19` |

### Judgement Purple (recolor)

| ID | Slot | Model1 | Model2 | ModelTexture1 | ModelTexture2 | Icon1 | Icon2 |
|----|------|--------|--------|---------------|---------------|-------|-------|
| 42853 | Shoulder | `palat2\plate_raidpaladint2_d_01_shoulder_l.mdx` | `palat2\plate_raidpaladint2_d_01_shoulder_r.mdx` | `palat2\plate_raidpaladint2_d_01_shoulder_purple` | `palat2\plate_raidpaladint2_d_01_shoulder_purple` | INV_Shoulder_37 | |
| 42863 | Chest | `palat2\plate_raidpaladint2_d_01_chest.mdx:2201` | | `palat2\plate_raidpaladint2_d_01_chest_purple` | | INV_Chest_Plate11 | `19` |
| 42862 | Belt | `plate_raidpaladint2_d_01_belt.mdx` | | `plate_raidpaladint2_d_01_belt_purple` | | INV_Belt_23 | `53::38` |
| 42851 | Bracers | | | | | INV_Bracer_13 | |
| 43639 | Gloves | `palat2\plate_raidpaladint2_d_01_chest.mdx:401` | `palat2\plate_raidpaladint2_d_01_chest.mdx:2301` | `palat2\plate_raidpaladint2_d_01_chest_purple` | `palat2\plate_raidpaladint2_d_01_chest_purple` | INV_Gauntlets_09 | `19:19` |
| 42859 | Legs | `palat2\plate_raidpaladint2_d_01_chest.mdx:1301` | | `palat2\plate_raidpaladint2_d_01_chest_purple` | | INV_Chest_Cloth_59 | `19` |
| 42864 | Boots | `palat2\plate_raidpaladint2_d_01_chest.mdx:2001` | | `palat2\plate_raidpaladint2_d_01_chest_purple` | | INV_Boots_Chain_08 | `19` |

**Notes on this set:**

- **Chest, Legs, Boots, Gloves** all load geosets from the same shared collection file. Attachment point `19` (Ground_Base) places them relative to the character root rather than a specific bone, which works because the collection M2 already has all bone transforms baked in.
- **Gloves** split into two geoset groups (`401` + `2301`) across Model1 and Model2, both on attachment `19:19`, letting each half of the mesh be textured or toggled independently.
- **Shoulders** use two separate M2 files for left (`_l`) and right (`_r`) pauldrons — the classic per-shoulder split. No Icon2 config needed; the defaults for the Shoulder slot already assign attachment 5 (right) to Model1 and attachment 6 (left) to Model2.
- **Belt** uses Icon2 `53::38` — attachment 53 (BeltBuckle), no attachment for Model2, flags `0x26` (`0x20 | 0x04 | 0x02`): model is in its own subfolder, no race or gender suffix appended.
- **Bracers** have no model entries. The bracer geometry is provided by the character's own skin geosets and does not need an attached M2.

---

## Known issues

**Dirty vanilla DBC entries:** Many stock 3.3.5a `ItemDisplayInfo.dbc` rows have `ModelName_1` or `ModelName_2` populated with values copied from other items (typically shoulders) that were never cleaned up because they had no visual effect in the base client. With this module active, those values will resolve and cause "missing model" placeholder boxes to appear on characters.

**Fix:** Clear `ModelName_1` and `ModelName_2` for any non-head/shoulder item that should not have an attached model. A cleanup script may be provided in a future release.

**Unequipping collections**
Unequipping does not clean up the attachment properly. I'll look into that soon.

---

## Future Plans

Support all slots, like neck/rings/trinkets/etc...

---

## Attachment point reference

The IDs below are the attachment slots present on retroported HD character models:

```c
typedef enum<uint32> {
    AT_Shield_MountMain_ItemVisual0 = 0,
    AT_HandRight_ItemVisual1        = 1,
    AT_HandLeft_ItemVisual2         = 2,
    AT_ElbowRight_ItemVisual3       = 3,
    AT_ElbowLeft_ItemVisual4        = 4,
    AT_Right_Shoulder               = 5,
    AT_Left_Shoulder                = 6,
    AT_Right_Knee                   = 7,
    AT_Left_Knee                    = 8,
    AT_HipRight                     = 9,
    AT_HipLeft                      = 10,
    AT_Helmet                       = 11,
    AT_Back                         = 12,
    AT_ShoulderFlapRight            = 13,
    AT_ShoulderFlapLeft             = 14,
    AT_Bust_ChestBloodFront         = 15,
    AT_Bust2_ChestBloodBack         = 16,
    AT_Face_Breath                  = 17,
    AT_AboveChar_PlayerName         = 18,
    AT_Ground_Base                  = 19,
    AT_Top_of_Head                  = 20,
    AT_SpellLeftHand                = 21,
    AT_SpellRightHand               = 22,
    AT_Special1                     = 23,
    AT_Special2                     = 24,
    AT_Special3                     = 25,
    AT_SheathMainHand               = 26,
    AT_SheathOffHand                = 27,
    AT_SheathShield                 = 28,
    AT_Belly_PlayerNameMounted      = 29,
    AT_LargeWeaponLeft              = 30,
    AT_LargeWeaponRight             = 31,
    AT_HipWeaponLeft                = 32,
    AT_HipWeaponRight               = 33,
    AT_Chest                        = 34,
    AT_HandArrow                    = 35,
    AT_Bullet                       = 36,
    AT_DemolisherVehicle1           = 37,
    AT_DemolisherVehicle2           = 38,
    AT_Vehicle_Seat1                = 39,
    AT_Vehicle_Seat2                = 40,
    AT_Vehicle_Seat3                = 41,
    AT_Vehicle_Seat4                = 42,
    AT_Vehicle_Seat5                = 43,
    AT_Vehicle_Seat6                = 44,
    AT_Vehicle_Seat7                = 45,
    AT_Vehicle_Seat8                = 46,
    AT_LeftFoot                     = 47,
    AT_RightFoot                    = 48,
    AT_ShieldNoGlove                = 49,
    AT_SpineLow                     = 50,
    AT_AlteredShoulderR             = 51,
    AT_AlteredShoulderL             = 52,
    AT_BeltBuckle                   = 53,
    AT_SheathCrossbow               = 54,
    AT_HeadTop                      = 55,
    AT_VirtualSpellDirected         = 56,
    AT_Backpack                     = 57,
    AT_Unk_58                       = 58,
    AT_Unk_59                       = 59,
    AT_Unk_60                       = 60,
} ATTACHMENT_ID;
```
