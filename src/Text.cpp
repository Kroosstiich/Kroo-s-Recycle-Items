#include "Text.h"

#include <unordered_map>

namespace KRI::Text
{
	namespace
	{
		struct StringHash
		{
			using is_transparent = void;
			std::size_t operator()(std::string_view a_text) const noexcept { return std::hash<std::string_view>{}(a_text); }
		};

		std::unordered_map<std::string, std::string, StringHash, std::equal_to<>> g_strings;

		std::string Language()
		{
			const auto ini = RE::INISettingCollection::GetSingleton();
			const auto setting = ini ? ini->GetSetting("sLanguage:General") : nullptr;
			return setting && setting->GetType() == RE::Setting::Type::kString && setting->data.s ? setting->data.s : "ENGLISH";
		}

		// UTF-16 LE with BOM, one "$key<TAB>text" per line, like every Skyrim translation file.
		bool Read(const std::string& a_language)
		{
			RE::BSResourceNiBinaryStream stream{ fmt::format("Interface\\Translations\\KroosRecycleItems_{}.txt", a_language) };
			if (!stream.good()) {
				return false;
			}
			std::wstring content;
			wchar_t      buffer[1024];
			for (;;) {
				std::uint64_t read = 0;
				stream.stream->DoRead(buffer, sizeof(buffer), read);
				if (read == 0) {
					break;
				}
				content.append(buffer, static_cast<std::size_t>(read / sizeof(wchar_t)));
			}
			if (content.starts_with(L'\xFEFF')) {
				content.erase(0, 1);
			}
			std::size_t start = 0;
			while (start < content.size()) {
				auto end = content.find(L'\n', start);
				if (end == std::wstring::npos) {
					end = content.size();
				}
				std::wstring_view line{ content.data() + start, end - start };
				start = end + 1;
				if (line.ends_with(L'\r')) {
					line.remove_suffix(1);
				}
				const auto tab = line.find(L'\t');
				if (!line.starts_with(L'$') || tab == std::wstring_view::npos) {
					continue;
				}
				auto key = SKSE::stl::utf16_to_utf8(line.substr(0, tab));
				auto value = SKSE::stl::utf16_to_utf8(line.substr(tab + 1));
				if (key && value) {
					g_strings.insert_or_assign(std::move(*key), std::move(*value));
				}
			}
			return true;
		}
	}

	void Load()
	{
		// English first, so a key missing from another language still reads as English.
		g_strings.clear();
		const auto language = Language();
		const bool english = Read("ENGLISH");
		const bool translated = language != "ENGLISH" && Read(language);
		if (!english && !translated) {
			SKSE::log::warn("Aucun fichier de traduction trouvé ({}, ENGLISH)", language);
			return;
		}
		SKSE::log::info("Traductions chargées ({}) : {} textes", language, g_strings.size());
	}

	const std::string& Get(std::string_view a_key)
	{
		if (const auto it = g_strings.find(a_key); it != g_strings.end()) {
			return it->second;
		}
		return g_strings.emplace(std::string{ a_key }, std::string{ a_key }).first->second;
	}

	std::string GameSetting(const char* a_name, const char* a_fallback)
	{
		const auto settings = RE::GameSettingCollection::GetSingleton();
		const auto setting = settings ? settings->GetSetting(a_name) : nullptr;
		return setting && setting->GetType() == RE::Setting::Type::kString && setting->data.s ? setting->data.s : a_fallback;
	}
}
