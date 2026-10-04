"""Write data/KroosRecycleItems.esl: a light master holding the MCM quest.

- CraftingCategories reads SKSE/Plugins/CraftingCategories/<plugin>.json only for loaded plugins.
- MCM Helper finds Data/MCM/Config/<plugin name>/config.json from the plugin that owns the quest.

The quest copies the layout of a working MCM Helper plugin (TrueHUD.esl): MCM_ConfigBase on the quest
(attached directly, so no Papyrus has to be compiled) and SKI_PlayerLoadGameAlias on a player alias.
"""
import pathlib
import struct

FORM_VERSION = 44
QUEST_ID = 0x01000800  # index 01 = this plugin (one master), ESL object id 0x800
PLAYER_REF = 0x00000014


def wstring(text: str) -> bytes:
    data = text.encode("cp1252")
    return struct.pack("<H", len(data)) + data


def zstring(text: str) -> bytes:
    return text.encode("cp1252") + b"\0"


def subrecord(tag: bytes, data: bytes) -> bytes:
    return tag + struct.pack("<H", len(data)) + data


def record(tag: bytes, flags: int, form_id: int, body: bytes) -> bytes:
    return tag + struct.pack("<IIIIHH", len(body), flags, form_id, 0, FORM_VERSION, 0) + body


def group(label: bytes, content: bytes) -> bytes:
    return b"GRUP" + struct.pack("<I", 24 + len(content)) + label + struct.pack("<iHHI", 0, 0, 0, 0) + content


def quest_vmad() -> bytes:
    vmad = struct.pack("<hhH", 5, 2, 1)  # version, object format, script count
    vmad += wstring("MCM_ConfigBase") + b"\x00" + struct.pack("<H", 1)  # status, property count
    vmad += wstring("ModName") + bytes([2, 1]) + wstring("$KRI_ModName")  # string, edited
    # Quest fragments: none; one alias carrying SkyUI's reload script.
    vmad += bytes([2]) + struct.pack("<H", 0) + wstring("")
    vmad += struct.pack("<H", 1)
    vmad += struct.pack("<HhI", 0, 0, QUEST_ID)  # alias 0 of this quest
    vmad += struct.pack("<hhH", 5, 2, 1)
    vmad += wstring("SKI_PlayerLoadGameAlias") + b"\x00" + struct.pack("<H", 0)
    return vmad


def quest() -> bytes:
    body = b"".join([
        subrecord(b"EDID", zstring("KRI_MCMQuest")),
        subrecord(b"VMAD", quest_vmad()),
        subrecord(b"FULL", zstring("KroosRecycleItems MCM")),
        subrecord(b"DNAM", bytes.fromhex("110100ff0000000000000000")),  # start game enabled, as TrueHUD
        subrecord(b"NEXT", b""),
        subrecord(b"ANAM", struct.pack("<I", 1)),
        subrecord(b"ALST", struct.pack("<I", 0)),
        subrecord(b"ALID", zstring("PlayerAlias")),
        subrecord(b"FNAM", struct.pack("<I", 0)),
        subrecord(b"ALFR", struct.pack("<I", PLAYER_REF)),
        subrecord(b"VTCK", struct.pack("<I", 0)),
        subrecord(b"ALED", b""),
    ])
    return record(b"QUST", 0, QUEST_ID, body)


def main() -> None:
    quests = group(b"QUST", quest())
    header_body = b"".join([
        subrecord(b"HEDR", struct.pack("<fiI", 1.71, 2, 0x801)),  # 1 group + 1 record
        subrecord(b"CNAM", zstring("Kroosstii")),
        subrecord(b"MAST", zstring("Skyrim.esm")),
        subrecord(b"DATA", struct.pack("<Q", 0)),
    ])
    header = record(b"TES4", 0x1 | 0x200, 0, header_body)  # master, light
    out = pathlib.Path(__file__).resolve().parent.parent / "data" / "KroosRecycleItems.esl"
    out.write_bytes(header + quests)
    print(f"{out} ({len(header) + len(quests)} bytes)")


if __name__ == "__main__":
    main()
