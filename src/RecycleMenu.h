#pragma once

namespace KRI::RecycleMenu
{
	// kDataLoaded, after the recipe scan: keywords and category tokens read by CraftingCategories.
	void CreateForms();

	// Put the "recovers: ..." line back on the item card after the list was rebuilt.
	void RefreshYieldLine();

	// Rebuild the list after a recycling, keeping the category the player was looking at.
	void RebuildList();

	// kPostLoad: every plugin has installed its hooks by then, so ours wraps CraftingCategories' one.
	void InstallHooks();
}
