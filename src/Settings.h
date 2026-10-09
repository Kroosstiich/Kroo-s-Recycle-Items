#pragma once

namespace KRI
{
	enum class Mode : std::int32_t
	{
		kRate = 0,      // a share of every material (default 10 %)
		kFull = 1,      // the whole recipe
		kMainOnly = 2,  // the whole main material (ingot, else leather), nothing else
		kOneOfEach = 3  // one unit of every material
	};

	// MCM Helper layout: defaults in Data/MCM/Config/KroosRecycleItems/settings.ini, the player's changes in
	// Data/MCM/Settings/KroosRecycleItems.ini (read second, wins). Percentages are written 0-100.
	struct Settings
	{
		Mode  mode{ Mode::kRate };
		bool  partial{ true };  // MCM toggle: kRate when on, otherwise fullMode + 1
		long  fullMode{ 0 };
		float rate{ 0.10f };
		float skillStart{ 15.0f };
		float skillGuarantee{ 50.0f };
		float skillMax{ 100.0f };
		float secondaryStart{ 0.01f };
		float secondaryMid{ 0.10f };
		float secondaryMax{ 0.20f };
		float experienceShare{ 0.10f };
		// Read when the game starts (the recipe scan): changing them in the MCM applies at the next launch.
		bool  clothLeather{ false };  // clothing without a recipe gives leather strips instead of linen
		bool  plateIron{ false };     // simple plates give iron instead of clay (Clutter.json "materialIron")
		bool  staves{ true };         // staves are recycled at the staff enchanter
		float enchantSmithing{ 50.0f };   // Smithing needed to recycle an enchanted item
		float enchantEnchanting{ 50.0f };  // Enchanting needed to recycle an enchanted item
		bool  staffNeedsLevels{ true };    // staves are enchanted by nature: the same levels apply to them

		// RECYCLING categories shown (weapons, armor, jewelry, clothing, miscellaneous items), and whether items
		// without a recipe of their own may borrow one from a similar item.
		std::array<bool, 5> categories{ true, true, true, true, true };
		bool                borrowedRecipes{ true };

		// Re-read both files; cheap, done every time a smelter or tanning rack opens.
		static void Load();

		[[nodiscard]] static const Settings& Get();
	};
}
