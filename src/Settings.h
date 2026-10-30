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

		// Recycling orders at a blacksmith (decided with Kevin on 2026-10-10).
		bool  ordersEnabled{ true };
		float orderShare{ 0.10f };     // price: share of the item's value...
		float orderMinPrice{ 5.0f };   // ...but at least this, per item (per lot for miscellaneous items)
		float orderHours{ 24.0f };     // in-game hours before the order is ready
		float orderSkill{ 50.0f };     // the blacksmith recycles like a player with this Smithing
		float holdHaafingar{ 1.5f };   // price factor per hold
		float holdReach{ 1.3f };
		float holdEastmarch{ 1.2f };
		float holdSolstheim{ 1.2f };
		float holdRift{ 1.1f };
		float holdStronghold{ 1.0f };
		float holdWhiterun{ 0.9f };
		float holdSmall{ 0.8f };       // Falkreath, the Pale, Hjaalmarch, Winterhold
		float discountOrc{ 0.40f };    // an orc in an orc stronghold
		float surchargeNonNord{ 0.30f };  // anyone but a Nord in Eastmarch
		float discountThieves{ 0.25f };   // Thieves Guild member in the Rift
		float discountThane{ 0.20f };     // thane of the hold
		float discountCompanions{ 0.20f };  // Companion at Eorlund's Skyforge
		float discountCivilWar{ 0.10f };    // Legion in Haafingar, Stormcloak in Eastmarch
		bool  spouseFree{ true };           // married to the blacksmith
		bool  orderReminders{ true };       // notifications about orders in progress
		float orderReminderHours{ 2.0f };   // game hours between two reminders

		// Re-read both files; cheap, done every time a smelter or tanning rack opens.
		static void Load();

		[[nodiscard]] static const Settings& Get();
	};
}
