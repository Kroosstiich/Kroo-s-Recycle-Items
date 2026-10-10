#include "Recycler.h"

#include "RecycleMenu.h"
#include "Settings.h"
#include "Text.h"

#include <random>

namespace KRI::Recycler
{
	namespace
	{
		using Family = RecipeScan::Family;
		using Station = RecipeScan::Station;


		float SkillLevel(RE::ActorValue a_skill)
		{
			const auto player = RE::PlayerCharacter::GetSingleton();
			return player ? player->AsActorValueOwner()->GetActorValue(a_skill) : 0.0f;
		}

		float Ramp(float a_skill, float a_from, float a_to)
		{
			return a_to > a_from ? std::clamp((a_skill - a_from) / (a_to - a_from), 0.0f, 1.0f) : (a_skill >= a_to ? 1.0f : 0.0f);
		}

		// Where the chances start rising, where the main material becomes certain, and where secondary materials
		// peak, for one item (decided with Kevin on 2026-10-04):
		// - plain items: Smithing, skillStart -> skillGuarantee -> skillMax;
		// - enchanted items: Smithing, shifted to start at the Smithing they require (50 -> 85 by default);
		// - staves: Enchanting, from the Enchanting they require up to a guarantee at skillMax (50 -> 100).
		struct Progression
		{
			float skill;
			float start;
			float guarantee;
			float max;

			[[nodiscard]] float SecondaryChance() const
			{
				const auto& settings = Settings::Get();
				if (skill <= guarantee) {
					return std::lerp(settings.secondaryStart, settings.secondaryMid, Ramp(skill, start, guarantee));
				}
				return std::lerp(settings.secondaryMid, settings.secondaryMax, Ramp(skill, guarantee, max));
			}
		};

		bool IsJewelry(const RE::TESBoundObject* a_item)
		{
			const auto armo = a_item->As<RE::TESObjectARMO>();
			return armo && armo->HasKeywordString("ArmorJewelry"sv);
		}

		// The copy to recycle: a plain one if any, otherwise the one with the fewest improvements. Worn copies
		// are never picked.
		struct Copy
		{
			RE::ExtraDataList* extra{ nullptr };
			bool               special{ false };  // enchanted, improved, renamed or favorite
			bool               enchanted{ false };
			bool               found{ false };
		};

		Copy PickCopy(RE::TESBoundObject* a_item)
		{
			Copy       best;
			const auto player = RE::PlayerCharacter::GetSingleton();
			if (!player) {
				return best;
			}
			auto inventory = player->GetInventory([a_item](RE::TESBoundObject& a_obj) { return &a_obj == a_item; });
			const auto it = inventory.find(a_item);
			if (it == inventory.end() || it->second.first <= 0 || !it->second.second || it->second.second->IsQuestObject()) {
				return best;
			}
			const auto& entry = it->second.second;
			std::int32_t inLists = 0;
			int          bestScore = INT_MAX;
			if (entry->extraLists) {
				for (const auto xList : *entry->extraLists) {
					if (!xList) {
						continue;
					}
					inLists += xList->GetCount();
					if (xList->HasType<RE::ExtraWorn>() || xList->HasType<RE::ExtraWornLeft>()) {
						continue;
					}
					const int score = xList->HasType<RE::ExtraEnchantment>() * 8 + xList->HasType<RE::ExtraHealth>() * 4 +
					                  xList->HasType<RE::ExtraTextDisplayData>() * 2 + xList->HasType<RE::ExtraHotkey>();
					if (score < bestScore) {
						bestScore = score;
						best = { xList, score > 0, xList->HasType<RE::ExtraEnchantment>(), true };
					}
				}
			}
			if (it->second.first - inLists > 0) {
				best = { nullptr, false, false, true };  // copies without extra data
			}
			// Items such as "Iron Sword of Burning" are enchanted on the base form, every copy alike.
			const auto enchantable = a_item->As<RE::TESEnchantableForm>();
			best.enchanted = best.enchanted || (enchantable && enchantable->formEnchanting);
			return best;
		}

		std::string Percent(float a_chance)
		{
			return Text::Format("$KRI_Percent", static_cast<int>(std::round(a_chance * 100.0f)));
		}

		bool IsStaff(const RE::TESBoundObject* a_item)
		{
			const auto weap = a_item->As<RE::TESObjectWEAP>();
			return weap && weap->GetWeaponType() == RE::WEAPON_TYPE::kStaff;
		}

		Progression ProgressionFor(const RE::TESBoundObject* a_item, const Copy& a_copy)
		{
			const auto& settings = Settings::Get();
			if (IsStaff(a_item)) {
				const float start = settings.staffNeedsLevels ? settings.enchantEnchanting : settings.skillStart;
				return { SkillLevel(RE::ActorValue::kEnchanting), start, settings.skillMax, settings.skillMax };
			}
			if (a_copy.enchanted) {
				const float start = settings.enchantSmithing;
				const float guarantee = start + (settings.skillGuarantee - settings.skillStart);
				return { SkillLevel(RE::ActorValue::kSmithing), start, guarantee, std::max(settings.skillMax, guarantee) };
			}
			return { SkillLevel(RE::ActorValue::kSmithing), settings.skillStart, settings.skillGuarantee, settings.skillMax };
		}

		// Enchanted items need Smithing and Enchanting (Kevin, 2026-10-04); staves only Enchanting, if the option
		// says so. A plain copy is always recycled first, so the levels only matter when none is left.
		bool LacksLevels(const RE::TESBoundObject* a_item, const Copy& a_copy)
		{
			const auto& settings = Settings::Get();
			if (IsStaff(a_item)) {
				return settings.staffNeedsLevels && SkillLevel(RE::ActorValue::kEnchanting) < settings.enchantEnchanting;
			}
			return a_copy.enchanted && (SkillLevel(RE::ActorValue::kSmithing) < settings.enchantSmithing ||
										   SkillLevel(RE::ActorValue::kEnchanting) < settings.enchantEnchanting);
		}

		// Plain copies (not worn, enchanted, improved, renamed or favorite) the player carries: "Recycle all"
		// only takes those, the special ones stay one at a time with their own warning.
		std::int32_t PlainCopies(RE::TESBoundObject* a_item)
		{
			const auto player = RE::PlayerCharacter::GetSingleton();
			if (!player) {
				return 0;
			}
			auto       inventory = player->GetInventory([a_item](RE::TESBoundObject& a_obj) { return &a_obj == a_item; });
			const auto it = inventory.find(a_item);
			if (it == inventory.end() || it->second.first <= 0 || !it->second.second || it->second.second->IsQuestObject()) {
				return 0;
			}
			std::int32_t plain = it->second.first;
			if (const auto& lists = it->second.second->extraLists) {
				for (const auto xList : *lists) {
					if (xList && (xList->HasType<RE::ExtraWorn>() || xList->HasType<RE::ExtraWornLeft>() || xList->HasType<RE::ExtraEnchantment>() ||
									 xList->HasType<RE::ExtraHealth>() || xList->HasType<RE::ExtraTextDisplayData>() || xList->HasType<RE::ExtraHotkey>())) {
						plain -= xList->GetCount();
					}
				}
			}
			return std::max(plain, 0);
		}

		// Recycle a_times lots of a_item (1 = the "Yes" button), each with its own rolls, then show one result box.
		void Recycle(RE::TESBoundObject* a_item, Station a_bench, std::int32_t a_times)
		{
			const auto player = RE::PlayerCharacter::GetSingleton();
			if (!player) {
				return;
			}
			const auto lot = RecipeScan::LotSize(a_item);
			const auto yields = ComputeYields(a_item, a_bench);

			static std::mt19937                       rng{ std::random_device{}() };
			std::uniform_real_distribution<float>     roll{ 0.0f, 1.0f };
			std::vector<std::pair<RE::TESBoundObject*, std::int32_t>> totals;
			float                                     experience = 0.0f;
			std::int32_t                              done = 0;
			bool                                      special = false;
			for (; done < a_times; ++done) {
				const auto copy = PickCopy(a_item);
				// "Recycle all" stops before the first special copy; a single recycle may take one.
				if (!copy.found || LacksLevels(a_item, copy) || (a_times > 1 && copy.special)) {
					break;
				}
				special = special || copy.special;
				player->RemoveItem(a_item, lot, RE::ITEM_REMOVE_REASON::kRemove, lot > 1 ? nullptr : copy.extra, nullptr);
				for (const auto& yield : yields) {
					const auto count = yield.guaranteed + (yield.chance > 0.0f && roll(rng) < yield.chance ? 1 : 0);
					if (count <= 0) {
						continue;
					}
					if (const auto it = std::ranges::find(totals, yield.material, &std::pair<RE::TESBoundObject*, std::int32_t>::first); it != totals.end()) {
						it->second += count;
					} else {
						totals.emplace_back(yield.material, count);
					}
				}
				if (const auto recipe = RecipeScan::ReferenceRecipe(a_item); recipe && recipe->createdItem) {
					experience += recipe->CalcSkillUse() * Settings::Get().experienceShare;
				}
			}
			if (done == 0) {
				SKSE::log::warn("Recyclage annulé : {} indisponible ou niveaux insuffisants", a_item->GetName());
				return;
			}

			std::string summary;
			for (const auto& [material, count] : totals) {
				player->AddObjectToContainer(material, nullptr, count, nullptr);
				summary += fmt::format("{}{} x{}", summary.empty() ? "" : ", ", material->GetName(), count);
			}
			if (experience > 0.0f) {
				player->AddSkillExperience(RE::ActorValue::kSmithing, experience);
			}

			const auto message = summary.empty() ? fmt::format("{} x{} recyclé : rien récupéré", a_item->GetName(), done * lot) :
			                                       fmt::format("{} x{} recyclé : {}", a_item->GetName(), done * lot, summary);
			SKSE::log::info("{} (XP Forge +{:.2f}, exemplaire {})", message, experience, special ? "spécial" : "simple");

			// The HUD is hidden behind the crafting menu: show the result in a box the player has to see.
			RE::BSString               text{ (summary.empty() ? Text::Get("$KRI_ResultNothing") : Text::Format("$KRI_Result", summary)).c_str() };
			RE::BSTArray<RE::BSString> buttons;
			buttons.push_back(RE::BSString{ Text::GameSetting("sOk", "OK").c_str() });
			RE::MessageBoxMenu::Create(text, RE::BSTSmartPointer<RE::IMessageBoxCallback>{}, 0, 4, 10, buttons);

			// Refresh the list so counts and emptied entries are up to date.
			RecycleMenu::RebuildList();
		}

		class ConfirmCallback : public RE::IMessageBoxCallback
		{
		public:
			ConfirmCallback(RE::TESBoundObject* a_item, Station a_bench, std::int32_t a_all) :
				_item(a_item),
				_bench(a_bench),
				_all(a_all)
			{}

			// Buttons: Yes, then "Recycle all" when offered (_all > 1), then No.
			void Run(std::uint8_t a_button) override
			{
				if (a_button == 0) {
					Recycle(_item, _bench, 1);
				} else if (a_button == 1 && _all > 1) {
					Recycle(_item, _bench, _all);
				}
			}

		private:
			RE::TESBoundObject* _item;
			Station             _bench;
			std::int32_t        _all;  // lots "Recycle all" takes; 0 or 1 = no such button
		};
	}

	std::vector<Yield> ComputeYields(const RE::TESBoundObject* a_item, Station a_bench, std::optional<float> a_skill)
	{
		std::vector<Yield> yields;
		const auto         ingredients = RecipeScan::IngredientsOf(a_item);
		const auto&        settings = Settings::Get();
		const auto         progression = a_skill ? Progression{ *a_skill, settings.skillStart, settings.skillGuarantee, settings.skillMax } :
		                                           ProgressionFor(a_item, PickCopy(const_cast<RE::TESBoundObject*>(a_item)));

		// Jewelry with a gem gives the gem only; otherwise the bench decides the main material. Items with
		// neither ingot nor leather (linen, dragon scales...) treat all their materials as main.
		const bool gemOnly = IsJewelry(a_item) && std::ranges::any_of(ingredients, [](const auto& i) { return i.family == Family::kGem; });
		const auto mainFamily = gemOnly                             ? Family::kGem :
		                        a_bench == Station::kTanningRack    ? Family::kLeather :
		                        a_bench == Station::kStaffEnchanter ? Family::kOther :
		                                                              Family::kMetal;
		const bool hasMain = std::ranges::any_of(ingredients, [&](const auto& i) { return i.family == mainFamily; });

		for (const auto& ingredient : ingredients) {
			if (gemOnly && ingredient.family != Family::kGem) {
				continue;
			}
			Yield yield{ ingredient.obj };
			yield.main = hasMain ? ingredient.family == mainFamily : true;

			// Whole-number part guaranteed, the rest as a chance of one more unit.
			auto split = [&](float a_expected) {
				yield.guaranteed = static_cast<std::int32_t>(std::floor(a_expected + 0.0001f));
				yield.chance = std::max(a_expected - yield.guaranteed, 0.0f);
				if (yield.chance < 0.005f) {
					yield.chance = 0.0f;
				}
			};

			switch (settings.mode) {
			case Mode::kFull:
				split(ingredient.count);
				break;
			case Mode::kMainOnly:
				if (!yield.main) {
					continue;
				}
				split(ingredient.count);
				break;
			case Mode::kOneOfEach:
				yield.guaranteed = 1;
				break;
			default:  // Mode::kRate
				if (!yield.main) {
					yield.chance = progression.SecondaryChance();
					break;
				}
				split(ingredient.count * settings.rate);
				if (yield.guaranteed == 0) {
					// Below one unit it is luck at first, then a sure unit from the guarantee step.
					yield.chance += (1.0f - yield.chance) * Ramp(progression.skill, progression.start, progression.guarantee);
					if (yield.chance >= 0.999f) {
						yield.guaranteed = 1;
						yield.chance = 0.0f;
					}
				}
				break;
			}
			if (yield.guaranteed > 0 || yield.chance > 0.0f) {
				yields.push_back(yield);
			}
		}
		return yields;
	}

	std::vector<std::string> YieldLabels(const RE::TESBoundObject* a_item, Station a_bench)
	{
		std::vector<std::string> labels;
		for (const auto& yield : ComputeYields(a_item, a_bench)) {
			const auto name = yield.material->GetName();
			if (yield.guaranteed > 0 && yield.chance > 0.0f) {
				labels.push_back(Text::Format("$KRI_YieldSurePlus", name, yield.guaranteed, Percent(yield.chance)));
			} else if (yield.guaranteed > 0) {
				labels.push_back(Text::Format("$KRI_YieldSure", name, yield.guaranteed));
			} else {
				labels.push_back(Text::Format("$KRI_YieldChance", name, Percent(yield.chance)));
			}
		}
		return labels;
	}

	std::string MissingLevels(const RE::TESBoundObject* a_item)
	{
		if (!LacksLevels(a_item, PickCopy(const_cast<RE::TESBoundObject*>(a_item)))) {
			return {};
		}
		const auto& settings = Settings::Get();
		return IsStaff(a_item) ? Text::Format("$KRI_ReqLevelsStaff", static_cast<int>(settings.enchantEnchanting)) :
		                         Text::Format("$KRI_ReqLevels", static_cast<int>(settings.enchantSmithing), static_cast<int>(settings.enchantEnchanting));
	}

	void Confirm(RE::TESBoundObject* a_item, Station a_bench)
	{
		const auto yields = ComputeYields(a_item, a_bench);
		const auto copy = PickCopy(a_item);

		if (LacksLevels(a_item, copy)) {
			const auto&                settings = Settings::Get();
			RE::BSString               text{ (IsStaff(a_item) ? Text::Format("$KRI_NeedLevelsStaff", static_cast<int>(settings.enchantEnchanting)) :
                                                    Text::Format("$KRI_NeedLevels", static_cast<int>(settings.enchantSmithing),
                                                        static_cast<int>(settings.enchantEnchanting)))
                                         .c_str() };
			RE::BSTArray<RE::BSString> buttons;
			buttons.push_back(RE::BSString{ Text::GameSetting("sOk", "OK").c_str() });
			RE::MessageBoxMenu::Create(text, RE::BSTSmartPointer<RE::IMessageBoxCallback>{}, 0, 4, 10, buttons);
			return;
		}

		const auto  lot = RecipeScan::LotSize(a_item);
		std::string body = fmt::format("{}\n\n{}\n", Text::Get("$KRI_Confirm"),
			lot > 1 ? Text::Format("$KRI_YieldSure", a_item->GetName(), lot) : std::string{ a_item->GetName() });
		if (copy.special) {
			body += Text::Get("$KRI_SpecialCopy") + "\n";
		}
		body += fmt::format("\n{}\n", Text::Get("$KRI_YieldHeader"));
		bool sure = false;
		for (const auto& yield : yields) {
			const auto name = yield.material->GetName();
			if (yield.guaranteed > 0 && yield.chance > 0.0f) {
				body += "- " + Text::Format("$KRI_YieldSurePlus", name, yield.guaranteed, Percent(yield.chance)) + "\n";
			} else if (yield.guaranteed > 0) {
				body += "- " + Text::Format("$KRI_YieldSure", name, yield.guaranteed) + "\n";
			} else {
				body += "- " + Text::Format("$KRI_YieldChanceLong", name, Percent(yield.chance)) + "\n";
			}
			sure = sure || yield.guaranteed > 0;
		}
		if (yields.empty()) {
			body += "- " + Text::Get("$KRI_Nothing") + "\n";
		}
		if (!sure) {
			body += "\n" + Text::Get("$KRI_MayGetNothing");
		}

		// "Recycle all" when at least two lots of plain copies are there (Kevin, 2026-10-09).
		const auto all = PlainCopies(a_item) / lot;

		RE::BSString         text{ body.c_str() };
		RE::BSTArray<RE::BSString> buttons;
		buttons.push_back(RE::BSString{ Text::GameSetting("sYes", "Yes").c_str() });
		if (all > 1) {
			buttons.push_back(RE::BSString{ Text::Format("$KRI_RecycleAll", all * lot).c_str() });
		}
		buttons.push_back(RE::BSString{ Text::GameSetting("sNo", "No").c_str() });
		const RE::BSTSmartPointer<RE::IMessageBoxCallback> callback{ new ConfirmCallback(a_item, a_bench, all) };
		RE::MessageBoxMenu::Create(text, callback, 0, 4, 10, buttons);
	}
}
