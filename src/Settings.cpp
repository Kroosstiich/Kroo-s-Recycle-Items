#include "Settings.h"

#include <SimpleIni.h>

namespace KRI
{
	namespace
	{
		Settings g_settings;

		void ReadFile(Settings& a_settings, const std::filesystem::path& a_path)
		{
			CSimpleIniA ini;
			ini.SetUnicode();
			if (ini.LoadFile(a_path.c_str()) < 0) {
				return;
			}
			auto percent = [&](const char* a_section, const char* a_key, float& a_value) {
				a_value = static_cast<float>(ini.GetDoubleValue(a_section, a_key, a_value * 100.0)) / 100.0f;
			};
			auto number = [&](const char* a_section, const char* a_key, float& a_value) {
				a_value = static_cast<float>(ini.GetDoubleValue(a_section, a_key, a_value));
			};
			// MCM: a "partial" toggle (greys out the share and progression settings when off) and the full mode.
			// Each file may set only one of the two keys, so both are kept and combined at the end.
			a_settings.partial = ini.GetBoolValue("General", "bPartial", a_settings.partial);
			a_settings.fullMode = std::clamp<long>(ini.GetLongValue("General", "iFullMode", a_settings.fullMode), 0, 2);
			a_settings.mode = a_settings.partial ? Mode::kRate : static_cast<Mode>(a_settings.fullMode + 1);
			percent("General", "fRate", a_settings.rate);
			percent("General", "fExperience", a_settings.experienceShare);
			a_settings.clothLeather = ini.GetLongValue("General", "iClothMaterial", a_settings.clothLeather ? 1 : 0) == 1;
			a_settings.plateIron = ini.GetLongValue("General", "iPlateMaterial", a_settings.plateIron ? 1 : 0) == 1;
			a_settings.staves = ini.GetBoolValue("General", "bStaves", a_settings.staves);
			number("Enchanted", "fSmithing", a_settings.enchantSmithing);
			number("Enchanted", "fEnchanting", a_settings.enchantEnchanting);
			a_settings.staffNeedsLevels = ini.GetBoolValue("Enchanted", "bStaffLevels", a_settings.staffNeedsLevels);
			number("Progression", "fSkillStart", a_settings.skillStart);
			number("Progression", "fSkillGuarantee", a_settings.skillGuarantee);
			number("Progression", "fSkillMax", a_settings.skillMax);
			percent("Progression", "fSecondaryStart", a_settings.secondaryStart);
			percent("Progression", "fSecondaryMid", a_settings.secondaryMid);
			percent("Progression", "fSecondaryMax", a_settings.secondaryMax);
		}
	}

	void Settings::Load()
	{
		Settings settings;
		ReadFile(settings, "Data/MCM/Config/KroosRecycleItems/settings.ini");
		ReadFile(settings, "Data/MCM/Settings/KroosRecycleItems.ini");

		// Keep the skill steps ordered so the chance ramps never divide by zero.
		settings.rate = std::clamp(settings.rate, 0.0f, 1.0f);
		settings.skillGuarantee = std::max(settings.skillGuarantee, settings.skillStart + 1.0f);
		settings.skillMax = std::max(settings.skillMax, settings.skillGuarantee + 1.0f);
		g_settings = settings;
	}

	const Settings& Settings::Get()
	{
		return g_settings;
	}
}
