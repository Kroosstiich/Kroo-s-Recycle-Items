#pragma once

namespace KRI::RecipeScan
{
	enum class Station
	{
		kSmelter,
		kTanningRack,
		kBoth,
		kNone,
		kStaffEnchanter
	};

	enum class Family
	{
		kMetal,
		kLeather,
		kGem,
		kOther
	};

	struct Ingredient
	{
		RE::TESBoundObject* obj;
		float               count;  // per unit of the item (recipes can make several)
		Family              family;
	};

	// Read every crafting recipe once the data is loaded, remember where each item is recycled and write a
	// coverage report to the log.
	void Run();

	// Where a_item is recycled; kNone when it is not recyclable.
	[[nodiscard]] Station StationOf(const RE::TESBoundObject* a_item);

	// How many copies of a_item are recycled together (miscellaneous items such as Dwemer forks); 1 otherwise.
	[[nodiscard]] std::int32_t LotSize(const RE::TESBoundObject* a_item);

	// The crafting recipe a_item is recycled from (its own, or its template's); nullptr when not recyclable.
	[[nodiscard]] const RE::BGSConstructibleObject* ReferenceRecipe(const RE::TESBoundObject* a_item);

	// True when a_item has no recipe of its own and borrows one from a similar item (same material and kind of
	// weapon or armor, or the default staff recipe).
	[[nodiscard]] bool IsBorrowed(const RE::TESBoundObject* a_item);

	// Ingots come out of a smelter, leather out of a tanning rack (hides count as leather), gems by keyword.
	[[nodiscard]] Family FamilyOf(const RE::TESBoundObject* a_material);

	// Materials of one a_item, coins removed and consumed equipment replaced by its own materials.
	[[nodiscard]] std::vector<Ingredient> IngredientsOf(const RE::TESBoundObject* a_item);
}
