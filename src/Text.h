#pragma once

// Player-facing strings, read from Interface/Translations/KroosRecycleItems_<LANGUAGE>.txt (the file Scaleform
// and MCM Helper already use). The DLL reads it itself: message boxes are built from several strings, and the
// game's translator may be replaced by another mod (ScaleformTranslationPP).
namespace KRI::Text
{
	void Load();

	// The translation of a_key ("$KRI_..."), or a_key itself when missing.
	[[nodiscard]] const std::string& Get(std::string_view a_key);

	// Get(a_key) with {0}, {1}... replaced; falls back to the raw text on a malformed translation.
	template <class... Args>
	[[nodiscard]] std::string Format(std::string_view a_key, Args&&... a_args)
	{
		const auto& text = Get(a_key);
		try {
			return fmt::format(fmt::runtime(text), std::forward<Args>(a_args)...);
		} catch (const fmt::format_error&) {
			return text;
		}
	}

	// A game setting string such as sYes / sNo / sOk, already in the game's language.
	[[nodiscard]] std::string GameSetting(const char* a_name, const char* a_fallback);
}
