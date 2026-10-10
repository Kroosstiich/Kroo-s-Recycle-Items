#pragma once

#include "RecipeScan.h"

namespace KRI::Recycler
{
	struct Yield
	{
		RE::TESBoundObject* material{ nullptr };
		std::int32_t        guaranteed{ 0 };
		float               chance{ 0.0f };  // chance of one more unit, 0..1
		bool                main{ false };
	};

	// Materials a_item gives back at a_bench, with the player's current Smithing skill, or as a plain item
	// recycled at a_skill (a blacksmith working on an order).
	[[nodiscard]] std::vector<Yield> ComputeYields(const RE::TESBoundObject* a_item, RecipeScan::Station a_bench,
		std::optional<float> a_skill = std::nullopt);

	// One label per material for the menu's ingredient line: "Lingot de fer (23 %)", "Lingot de fer x2".
	[[nodiscard]] std::vector<std::string> YieldLabels(const RE::TESBoundObject* a_item, RecipeScan::Station a_bench);

	// The levels the player still lacks to recycle a_item ("Forge 50, Enchantement 50"), or empty when none.
	[[nodiscard]] std::string MissingLevels(const RE::TESBoundObject* a_item);

	// Ask for confirmation, then recycle one copy of a_item (the least valuable one the player carries).
	void Confirm(RE::TESBoundObject* a_item, RecipeScan::Station a_bench);
}
