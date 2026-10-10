#pragma once

// Recycling orders at a blacksmith (Kevin, 2026-10-10). The player asks any blacksmith, drops the items into a
// deposit box, pays, and comes back a day later for the materials. No experience, no perks: the blacksmith
// works as a Smithing 50 player would. Prices depend on the hold and on who the player is.
//
// Dialogue lives in KroosRecycleItems.esl (see tools/make-esl.py); each blacksmith's order state is his rank in
// KRI_OrderFaction (none = no order, 0 = in progress, 1 = ready), which the dialogue conditions read.
namespace KRI::Orders
{
	// Find the plugin's forms, translate the topics and register the event sinks. Once the data is loaded.
	void Init();

	// Show or hide the order topic as the MCM says (a global the dialogue condition reads).
	void ApplySettings();

	// SKSE co-save: the orders in progress.
	void RegisterSerialization();
}
