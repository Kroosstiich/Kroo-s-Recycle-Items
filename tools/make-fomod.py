"""Build the release archive: build/package/ (FOMOD layout) and build/KroosRecycleItems-<version>.zip.

Run after tools/build.ps1. The installer asks the language first, then shows its pages in that language.
Each combination of choices installs its own MCM Helper settings.ini, so the MCM's defaults are the
choices made at install time.
"""
import itertools
import re
import shutil
import zipfile
from pathlib import Path
from xml.sax.saxutils import escape

ROOT = Path(__file__).resolve().parent.parent
DATA = ROOT / "data"
BUILD = ROOT / "build"
PACKAGE = BUILD / "package"
VERSION = re.search(r"project\(KroosRecycleItems VERSION ([\d.]+)", (ROOT / "CMakeLists.txt").read_text()).group(1)

IMAGE = r"fomod\images\banner.jpg"  # Nexus banner (tools/banner.png), resized
GAME_LANGUAGES = ["ENGLISH", "FRENCH", "GERMAN", "ITALIAN", "SPANISH", "POLISH", "RUSSIAN", "JAPANESE", "CHINESE"]
SETTINGS_INI = "MCM/Config/KroosRecycleItems/settings.ini"

# Choices: flag value -> ini keys. The first one is the default.
MODES = {
    "p10": {"bPartial": "1", "fRate": "10"},
    "p25": {"bPartial": "1", "fRate": "25"},
    "p50": {"bPartial": "1", "fRate": "50"},
    "full": {"bPartial": "0", "iFullMode": "0"},
    "main": {"bPartial": "0", "iFullMode": "1"},
    "each": {"bPartial": "0", "iFullMode": "2"},
}
CLOTHS = {"linen": {"iClothMaterial": "0"}, "strips": {"iClothMaterial": "1"}}
STAVES = {"on": {"bStaves": "1"}, "off": {"bStaves": "0"}}
STAFF_LEVELS = {"on": {"bStaffLevels": "1"}, "off": {"bStaffLevels": "0"}}
PLATES = {"clay": {"iPlateMaterial": "0"}, "iron": {"iPlateMaterial": "1"}}

MCM_NOTE = {
    "fr": "Tu pourras modifier ce réglage à tout moment dans le MCM.",
    "en": "You can change this setting at any time in the MCM.",
}
RESTART_NOTE = {
    "fr": "Tu pourras modifier ce réglage à tout moment dans le MCM ; il sera pris en compte au prochain lancement du jeu.",
    "en": "You can change this setting at any time in the MCM; it applies at the next game launch.",
}

# Pages, per language: (step name, [(group name, flag, [(value, label, description, dependency)])]).
# dependency: None, or "dragonborn" (Recommended when Dragonborn.esm is active, unusable otherwise).
TEXT = {
    "fr": [
        ("Mode de recyclage", [("Ce que rend un objet recyclé", "kri_mode", [
            ("p10", "Une part : 10 % (par défaut)",
             "Chaque ressource de la recette est rendue à 10 %. Ce qui ne fait pas une unité entière devient une "
             "chance d'en obtenir une. Les chances montent avec la Forge : à partir du niveau 50, au moins une "
             "unité de la ressource principale est garantie.", None),
            ("p25", "Une part : 25 %", "Comme le mode par défaut, mais chaque ressource est rendue à 25 %.", None),
            ("p50", "Une part : 50 %", "Comme le mode par défaut, mais chaque ressource est rendue à 50 %.", None),
            ("full", "Toutes les ressources (100 %)",
             "L'objet rend toute sa recette. La Forge ne change rien aux quantités.", None),
            ("main", "Toute la ressource principale, rien d'autre",
             "L'objet rend toute sa ressource principale, par exemple les lingots de fer d'une épée en fer, mais "
             "pas les bandes de cuir.", None),
            ("each", "Une unité de chaque ressource",
             "L'objet rend une unité de chaque ressource de sa recette.", None),
        ])]),
        ("Vêtements", [("Ressource des vêtements sans recette", "kri_cloth", [
            ("linen", "Lin (par défaut)",
             "Les robes, les capuches et les autres vêtements rendent du lin. Les gants et les bottes rendent des "
             "bandes de cuir.", None),
            ("strips", "Bandes de cuir", "Tous les vêtements sans recette rendent des bandes de cuir.", None),
        ])]),
        ("Assiettes simples", [("Ressource des assiettes simples", "kri_plates", [
            ("clay", "Argile (par défaut)",
             "Les assiettes simples et la vaisselle émaillée se recyclent à la fonderie, par lot de 2, et rendent "
             "1 argile.", None),
            ("iron", "Lingot de fer",
             "Les assiettes simples et la vaisselle émaillée se recyclent à la fonderie, par lot de 2, et rendent "
             "1 lingot de fer.", None),
        ])]),
        ("Bâtons", [
            ("Recyclage des bâtons", "kri_staves", [
                ("on", "Recycler les bâtons",
                 "Les bâtons se recyclent à la table d'enchantement de bâtons de Solstheim et rendent des pierres de "
                 "cœur. Nécessite Dragonborn.", "dragonborn"),
                ("off", "Ne pas recycler les bâtons", "Les bâtons n'apparaissent pas dans la section RECYCLAGE.", None),
            ]),
            ("Niveau requis pour les bâtons", "kri_stafflv", [
                ("on", "Enchantement 50 requis (par défaut)",
                 "Un bâton se recycle à partir d'Enchantement 50. Ses chances montent ensuite avec l'Enchantement, "
                 "jusqu'à être garanties au niveau 100. La Forge ne compte pas.", None),
                ("off", "Aucune condition",
                 "Un bâton se recycle à tout niveau. Ses chances montent avec l'Enchantement.", None),
            ]),
        ]),
    ],
    "en": [
        ("Recycling mode", [("What a recycled item gives back", "kri_mode", [
            ("p10", "A share: 10% (default)",
             "Each material of the recipe is given back at 10%. Anything short of a whole unit becomes a chance to "
             "get one. Chances rise with Smithing: from level 50, at least one unit of the main material is "
             "guaranteed.", None),
            ("p25", "A share: 25%", "Like the default mode, but each material is given back at 25%.", None),
            ("p50", "A share: 50%", "Like the default mode, but each material is given back at 50%.", None),
            ("full", "All materials (100%)", "The item gives back its whole recipe. Smithing does not change the "
             "amounts.", None),
            ("main", "All of the main material, nothing else",
             "The item gives back all of its main material, such as the iron ingots of an iron sword, but not the "
             "leather strips.", None),
            ("each", "One unit of each material", "The item gives back one unit of each material of its recipe.",
             None),
        ])]),
        ("Clothing", [("Material of clothing without a recipe", "kri_cloth", [
            ("linen", "Linen (default)",
             "Robes, hoods and other clothes give back linen. Gloves and boots give back leather strips.", None),
            ("strips", "Leather strips", "All clothing without a recipe gives back leather strips.", None),
        ])]),
        ("Simple plates", [("Material of simple plates", "kri_plates", [
            ("clay", "Clay (default)",
             "Simple plates and glazed pottery are recycled at the smelter, 2 at a time, and give back 1 clay.", None),
            ("iron", "Iron ingot",
             "Simple plates and glazed pottery are recycled at the smelter, 2 at a time, and give back 1 iron "
             "ingot.", None),
        ])]),
        ("Staves", [
            ("Staff recycling", "kri_staves", [
                ("on", "Recycle staves",
                 "Staves are recycled at the Solstheim staff enchanter and give back heart stones. Requires "
                 "Dragonborn.", "dragonborn"),
                ("off", "Do not recycle staves", "Staves do not appear in the RECYCLING section.", None),
            ]),
            ("Level needed for staves", "kri_stafflv", [
                ("on", "Enchanting 50 required (default)",
                 "A staff can be recycled from Enchanting 50. Its chances then rise with Enchanting until certain "
                 "at level 100. Smithing does not matter.", None),
                ("off", "No requirement", "A staff can be recycled at any level. Its chances rise with Enchanting.",
                 None),
            ]),
        ]),
    ],
}
# Settings read when the game starts (the recipe scan): an MCM change applies at the next launch.
AT_LAUNCH = {"kri_cloth", "kri_plates", "kri_staves"}


def settings_ini(choice):
    text = (DATA / SETTINGS_INI).read_text(encoding="utf-8")
    for key, value in choice.items():
        text, n = re.subn(rf"^{key} = .*$", f"{key} = {value}", text, flags=re.M)
        assert n == 1, key
    return text


def plugin_xml(lang, flag, value, label, description, dependency):
    note = "" if flag == "kri_lang" else RESTART_NOTE[lang] if flag in AT_LAUNCH else MCM_NOTE[lang]
    if note:
        note = "&#13;&#10;&#13;&#10;" + escape(note)
    if dependency == "dragonborn":
        type_xml = """<typeDescriptor><dependencyType><defaultType name="NotUsable"/><patterns><pattern>
                <dependencies operator="And"><fileDependency file="Dragonborn.esm" state="Active"/></dependencies>
                <type name="Recommended"/></pattern></patterns></dependencyType></typeDescriptor>"""
    else:
        type_xml = '<typeDescriptor><type name="Optional"/></typeDescriptor>'
    return f"""
            <plugin name="{escape(label)}">
              <description>{escape(description)}{note}</description>
              <image path="{IMAGE}"/>
              <conditionFlags><flag name="{flag}">{value}</flag></conditionFlags>
              {type_xml}
            </plugin>"""


def group_xml(lang, name, flag, options):
    plugins = "".join(plugin_xml(lang, flag, *option) for option in options)
    return f"""
        <group name="{escape(name)}" type="SelectExactlyOne">
          <plugins order="Explicit">{plugins}
          </plugins>
        </group>"""


def step_xml(name, groups, lang=None):
    visible = (f'\n      <visible><flagDependency flag="kri_lang" value="{lang}"/></visible>' if lang else "")
    body = "".join(group_xml(lang, *group) for group in groups)
    return f"""
    <installStep name="{escape(name)}">{visible}
      <optionalFileGroups order="Explicit">{body}
      </optionalFileGroups>
    </installStep>"""


def flags_xml(flags):
    deps = "".join(f'<flagDependency flag="{flag}" value="{value}"/>' for flag, value in flags)
    return f'<dependencies operator="And">{deps}</dependencies>'


def main():
    dll = BUILD / "release" / "KroosRecycleItems.dll"
    assert dll.exists(), "build first: tools/build.ps1"
    if PACKAGE.exists():
        shutil.rmtree(PACKAGE)

    # Files every install gets.
    core = PACKAGE / "core"
    shutil.copytree(DATA, core, ignore=shutil.ignore_patterns("settings.ini", "Translations"))
    (core / "SKSE" / "Plugins").mkdir(parents=True, exist_ok=True)
    shutil.copy2(dll, core / "SKSE" / "Plugins")

    patterns = []
    # The language chosen on the first page is the mod's language too, whatever the game's language: its texts
    # are installed under every game language's file name. A translation mod can still overwrite one of them.
    for lang, source in (("en", "ENGLISH"), ("fr", "FRENCH")):
        folder = f"options/lang-{lang}"
        target = PACKAGE / folder / "Interface" / "Translations"
        target.mkdir(parents=True)
        for game_language in GAME_LANGUAGES:
            shutil.copy2(DATA / "Interface" / "Translations" / f"KroosRecycleItems_{source}.txt",
                         target / f"KroosRecycleItems_{game_language}.txt")
        patterns.append(([("kri_lang", lang)], folder))
    for (mode, m), (cloth, c), (plates, p), (staves, s), (lv, l) in itertools.product(
            MODES.items(), CLOTHS.items(), PLATES.items(), STAVES.items(), STAFF_LEVELS.items()):
        folder = f"options/settings/{mode}-{cloth}-{plates}-staves{staves}-levels{lv}"
        target = PACKAGE / folder / SETTINGS_INI
        target.parent.mkdir(parents=True)
        target.write_text(settings_ini(m | c | p | s | l), encoding="utf-8")
        flags = [("kri_mode", mode), ("kri_cloth", cloth), ("kri_plates", plates), ("kri_staves", staves),
                 ("kri_stafflv", lv)]
        patterns.append((flags, folder))

    language = step_xml("Language / Langue", [("Installer language / Langue de l'installateur", "kri_lang", [
        ("en", "English", "The installer and the mod in English, whatever the game's language.", None),
        ("fr", "Français", "L'installateur et le mod en français, quelle que soit la langue du jeu.", None),
    ])])
    steps = language + "".join(step_xml(name, groups, lang) for lang in ("fr", "en") for name, groups in TEXT[lang])

    installs = "".join(f"""
      <pattern>{flags_xml(flags)}<files><folder source="{folder}" destination="" priority="0"/></files></pattern>"""
                       for flags, folder in patterns)

    fomod = PACKAGE / "fomod"
    fomod.mkdir()
    # The Nexus banner, smaller, shown next to every option.
    from PIL import Image
    (fomod / "images").mkdir()
    banner = Image.open(Path(__file__).with_name("banner.png")).convert("RGB")
    banner.thumbnail((960, 540), Image.LANCZOS)
    banner.save(fomod / "images" / "banner.jpg", quality=90)
    (fomod / "ModuleConfig.xml").write_text(f"""<?xml version="1.0" encoding="utf-8"?>
<config xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance" xsi:noNamespaceSchemaLocation="http://qconsulting.ca/fo3/ModConfig5.0.xsd">
  <moduleName>Kroo's Recycle Items</moduleName>
  <moduleImage path="{IMAGE}"/>
  <requiredInstallFiles>
    <folder source="core" destination=""/>
  </requiredInstallFiles>
  <installSteps order="Explicit">{steps}
  </installSteps>
  <conditionalFileInstalls>
    <patterns>{installs}
    </patterns>
  </conditionalFileInstalls>
</config>
""", encoding="utf-8")
    (fomod / "info.xml").write_text(f"""<?xml version="1.0" encoding="utf-8"?>
<fomod>
  <Name>Kroo's Recycle Items</Name>
  <Author>Kroosstii</Author>
  <Version>{VERSION}</Version>
  <Description>Recycle your items at the smelter, the tanning rack and the staff enchanter to get materials back.</Description>
  <Groups><element>Gameplay</element></Groups>
</fomod>
""", encoding="utf-8")

    archive = BUILD / f"KroosRecycleItems-{VERSION}.zip"
    with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED) as zf:
        for path in sorted(PACKAGE.rglob("*")):
            if path.is_file():
                zf.write(path, path.relative_to(PACKAGE).as_posix())
    print(f"{archive} ({archive.stat().st_size // 1024} KB, {len(patterns)} conditional installs)")


if __name__ == "__main__":
    main()
