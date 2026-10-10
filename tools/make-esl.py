"""Write data/KroosRecycleItems.esl and data/SEQ/KroosRecycleItems.seq: a light master holding the MCM quest
and the blacksmith order dialogue.

- CraftingCategories reads SKSE/Plugins/CraftingCategories/<plugin>.json only for loaded plugins.
- MCM Helper finds Data/MCM/Config/<plugin name>/config.json from the plugin that owns the quest.

The quest copies the layout of a working MCM Helper plugin (TrueHUD.esl): MCM_ConfigBase on the quest
(attached directly, so no Papyrus has to be compiled) and SKI_PlayerLoadGameAlias on a player alias.

Blacksmith orders (Kevin, 2026-10-10): two top-level player topics on every JobBlacksmithFaction member and one
topic the DLL makes the blacksmith say. Their lines reuse vanilla shared infos, so every blacksmith answers in
his own voice and in the game's language without any copied sound file. The DLL reacts to the lines
(TESTopicInfoEvent) and keeps each blacksmith's order state as his rank in KRI_OrderFaction: none = no order,
0 = in progress, 1 = ready. Record layouts copied from Skyrim.esm (DialogueFavorGenericBribeBranch,
MarriageAskedFaction, BarrelEmpty01).
"""
import pathlib
import struct

FORM_VERSION = 44
QUEST_ID = 0x01000800  # index 01 = this plugin (one master), ESL object id 0x800
PLAYER_REF = 0x00000014

# Records of this plugin (index 01, light master object ids from 0x800).
ORDER_FACTION = 0x01000801
ORDER_BRANCH, ORDER_TOPIC, ORDER_INFO = 0x01000802, 0x01000803, 0x01000804
PICKUP_BRANCH, PICKUP_TOPIC, PICKUP_INFO = 0x01000805, 0x01000806, 0x01000807
PAID_TOPIC, PAID_INFO = 0x01000808, 0x01000809
ORDER_CHEST = 0x0100080A
ORDERS_ENABLED = 0x0100080B  # global set by the DLL from the MCM
NEXT_OBJECT_ID = 0x80C

# Skyrim.esm forms.
JOB_BLACKSMITH = 0x0005091D
SHARED_OF_COURSE = 0x000DBA22       # "Of course." / "Bien entendu."
SHARED_HERE_FOR_YOU = 0x000DBA2A    # "Thank you. Here, this is for you." / "Merci. Tenez, c'est pour vous."
SHARED_FOR_THAT_PRICE = 0x000E0CC2  # "For that price? Sure." / "Pour ce prix-là ? Bien sûr."

GET_IN_FACTION, GET_FACTION_RANK, GET_GLOBAL_VALUE = 71, 73, 74
EQ, LT = 0, 4  # condition operators (top three bits of the first CTDA byte)


def wstring(text: str) -> bytes:
    data = text.encode("cp1252")
    return struct.pack("<H", len(data)) + data


def zstring(text: str) -> bytes:
    return text.encode("cp1252") + b"\0"


def subrecord(tag: bytes, data: bytes) -> bytes:
    return tag + struct.pack("<H", len(data)) + data


def record(tag: bytes, flags: int, form_id: int, body: bytes) -> bytes:
    return tag + struct.pack("<IIIIHH", len(body), flags, form_id, 0, FORM_VERSION, 0) + body


def group(label: bytes, content: bytes, group_type: int = 0) -> bytes:
    return b"GRUP" + struct.pack("<I", 24 + len(content)) + label + struct.pack("<iHHI", group_type, 0, 0, 0) + content


def condition(function: int, param: int, operator: int, value: float) -> bytes:
    """CTDA run on the speaker (Subject)."""
    return subrecord(b"CTDA", struct.pack("<B3xfHHIIIIi", operator << 5, value, function, 0, param, 0, 0, 0, -1))


def enabled_global() -> bytes:
    return record(b"GLOB", 0, ORDERS_ENABLED, b"".join([
        subrecord(b"EDID", zstring("KRI_OrdersEnabled")),
        subrecord(b"FNAM", b"s"),
        subrecord(b"FLTV", struct.pack("<f", 1.0)),
    ]))


def faction() -> bytes:
    return record(b"FACT", 0, ORDER_FACTION, b"".join([
        subrecord(b"EDID", zstring("KRI_OrderFaction")),
        subrecord(b"DATA", struct.pack("<I", 1)),  # hidden from the player
        subrecord(b"CRVA", bytes.fromhex("0101000000000000000000000000000000000000")),
        subrecord(b"VENV", bytes(12)),
    ]))


def chest() -> bytes:
    # Deposit box for an order: spawned out of sight by the DLL, renamed in the game's language.
    return record(b"CONT", 0, ORDER_CHEST, b"".join([
        subrecord(b"EDID", zstring("KRI_OrderChest")),
        subrecord(b"OBND", bytes.fromhex("e5ffe5ff00001b001b005000")),
        subrecord(b"FULL", zstring("Recycling order")),
        subrecord(b"MODL", zstring("Clutter\\Barrel01.NIF")),
        subrecord(b"DATA", bytes(5)),
    ]))


def branch(form_id: int, editor_id: str, topic_id: int) -> bytes:
    return record(b"DLBR", 0, form_id, b"".join([
        subrecord(b"EDID", zstring(editor_id)),
        subrecord(b"QNAM", struct.pack("<I", QUEST_ID)),
        subrecord(b"TNAM", struct.pack("<I", 0)),  # player
        subrecord(b"DNAM", struct.pack("<I", 1)),  # top level
        subrecord(b"SNAM", struct.pack("<I", topic_id)),
    ]))


def topic(form_id: int, editor_id: str, text: str, branch_id: int, line: bytes) -> bytes:
    """A custom topic and its single line; the DLL replaces the English text with the game's language."""
    dial = record(b"DIAL", 0, form_id, b"".join([
        subrecord(b"EDID", zstring(editor_id)),
        subrecord(b"FULL", zstring(text)),
        subrecord(b"PNAM", struct.pack("<f", 50.0)),
        *([subrecord(b"BNAM", struct.pack("<I", branch_id))] if branch_id else []),
        subrecord(b"QNAM", struct.pack("<I", QUEST_ID)),
        subrecord(b"DATA", struct.pack("<BBH", 0, 0, 0)),  # topic category, custom subtype
        subrecord(b"SNAM", b"CUST"),
        subrecord(b"TIFC", struct.pack("<I", 1)),
    ]))
    return dial + group(struct.pack("<I", form_id), line, 7)


def info(form_id: int, shared: int, goodbye: bool, conditions: list) -> bytes:
    return record(b"INFO", 0, form_id, b"".join([
        subrecord(b"ENAM", struct.pack("<HH", 1 if goodbye else 0, 0)),
        subrecord(b"CNAM", bytes(1)),
        subrecord(b"DNAM", struct.pack("<I", shared)),
        *conditions,
    ]))


def dialogue() -> tuple:
    blacksmith = condition(GET_IN_FACTION, JOB_BLACKSMITH, EQ, 1.0)
    topics = b"".join([
        topic(ORDER_TOPIC, "KRI_OrderTopic", "I have some items for you to recycle.", ORDER_BRANCH,
              info(ORDER_INFO, SHARED_OF_COURSE, True, [blacksmith, condition(GET_FACTION_RANK, ORDER_FACTION, LT, 0.0),
                                                        condition(GET_GLOBAL_VALUE, ORDERS_ENABLED, EQ, 1.0)])),
        topic(PICKUP_TOPIC, "KRI_PickupTopic", "Is my order ready?", PICKUP_BRANCH,
              info(PICKUP_INFO, SHARED_HERE_FOR_YOU, True, [blacksmith, condition(GET_FACTION_RANK, ORDER_FACTION, EQ, 1.0)])),
        # Said by the blacksmith once the order is paid (Papyrus Say, called by the DLL).
        topic(PAID_TOPIC, "KRI_PaidTopic", "Order paid", 0, info(PAID_INFO, SHARED_FOR_THAT_PRICE, False, [blacksmith])),
    ])
    branches = branch(ORDER_BRANCH, "KRI_OrderBranch", ORDER_TOPIC) + branch(PICKUP_BRANCH, "KRI_PickupBranch", PICKUP_TOPIC)
    return topics, branches


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
    topics, branches = dialogue()
    # Top-level groups in the game's usual order: GLOB, FACT, CONT, DIAL, QUST, DLBR.
    body = (group(b"GLOB", enabled_global()) + group(b"FACT", faction()) + group(b"CONT", chest()) + group(b"DIAL", topics) + group(b"QUST", quest()) +
            group(b"DLBR", branches))
    records = 12 + 6 + 3  # GLOB, FACT, CONT, 3 DIAL, 3 INFO, QUST, 2 DLBR + 6 top-level groups + 3 topic children groups
    header_body = b"".join([
        subrecord(b"HEDR", struct.pack("<fiI", 1.71, records, NEXT_OBJECT_ID)),
        subrecord(b"CNAM", zstring("Kroosstii")),
        subrecord(b"MAST", zstring("Skyrim.esm")),
        subrecord(b"DATA", struct.pack("<Q", 0)),
    ])
    header = record(b"TES4", 0x1 | 0x200, 0, header_body)  # master, light
    data = pathlib.Path(__file__).resolve().parent.parent / "data"
    out = data / "KroosRecycleItems.esl"
    out.write_bytes(header + body)
    # The start-game-enabled quest carries dialogue: without its .seq entry it never starts.
    seq = data / "SEQ" / "KroosRecycleItems.seq"
    seq.parent.mkdir(exist_ok=True)
    seq.write_bytes(struct.pack("<I", QUEST_ID))
    print(f"{out} ({len(header) + len(body)} bytes), {seq}")


if __name__ == "__main__":
    main()
