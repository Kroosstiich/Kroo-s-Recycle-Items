#include "RecipeScan.h"

#include "Settings.h"

#include <fstream>
#include <map>

#include <nlohmann/json.hpp>
#include <unordered_map>
#include <unordered_set>

namespace KRI::RecipeScan
{
	namespace
	{
		// Benches whose recipes create equipment. Smelter recipes are breakdown recipes, armor table and
		// sharpening wheel recipes are tempering: neither describes what an item is made of.
		constexpr std::array kCreationBenches{ "CraftingSmithingForge"sv, "CraftingSmithingSkyforge"sv, "CraftingTanningRack"sv,
			"DLC2StaffEnchanter"sv };

		constexpr RE::FormID kGold = 0x0000000F;

		bool IsSoulGemFragment(const RE::TESForm* a_form)
		{
			// Vanilla fragments carry no dedicated keyword; the English and French names both say so.
			std::string name = a_form->GetName();
			std::ranges::transform(name, name.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			return name.contains("soul gem") || name.contains("gemme d'âme") || name.contains("gemmes d'âme");
		}

		struct ItemData
		{
			Station                           station;
			const RE::BGSConstructibleObject* recipe;
			bool                              borrowed{ false };  // recipe of a similar item (see IsBorrowed)
		};

		std::unordered_map<const RE::TESBoundObject*, ItemData> g_items;
		std::unordered_map<const RE::TESBoundObject*, std::int32_t> g_lots;  // miscellaneous items recycled by lots

		struct Recipe
		{
			const RE::BGSConstructibleObject* cobj{ nullptr };
			std::int32_t                      total{ 0 };
		};

		// Cheapest creation recipe of each armor / weapon that has one.
		std::unordered_map<const RE::TESForm*, Recipe> g_direct;

		struct Materials
		{
			std::unordered_set<const RE::TESForm*> metal;    // created at a smelter
			std::unordered_set<const RE::TESForm*> leather;  // created at a tanning rack
			std::unordered_set<const RE::TESForm*> hides;    // turned into leather at a tanning rack

			Family FamilyOf(const RE::TESBoundObject* a_obj) const
			{
				if (metal.contains(a_obj)) {
					return Family::kMetal;
				}
				if (leather.contains(a_obj) || hides.contains(a_obj)) {
					return Family::kLeather;
				}
				const auto misc = a_obj->As<RE::TESObjectMISC>();
				if (misc && misc->HasKeywordString("VendorItemGem"sv)) {
					return Family::kGem;
				}
				return Family::kOther;
			}
		};

		Materials g_mats;

		std::string_view BenchId(const RE::BGSConstructibleObject* a_cobj)
		{
			if (!a_cobj->benchKeyword) {
				return "<aucun>"sv;
			}
			const char* id = a_cobj->benchKeyword->GetFormEditorID();
			return id && *id ? std::string_view{ id } : "<sans EditorID>"sv;
		}

		bool IsCreationBench(std::string_view a_bench)
		{
			return std::ranges::find(kCreationBenches, a_bench) != kCreationBenches.end();
		}

		std::string Describe(const RE::TESForm* a_form)
		{
			const char* name = a_form->GetName();
			return fmt::format("{} [{:08X}]", name && *name ? name : "<sans nom>", a_form->GetFormID());
		}

		std::string_view FileOf(const RE::TESForm* a_form)
		{
			const auto file = a_form->GetFile(0);
			return file ? file->GetFilename() : "<dynamique>"sv;
		}

		std::string DescribeRecipe(const RE::BGSConstructibleObject* a_cobj, const Materials& a_mats)
		{
			std::string out = fmt::format("[{}] x{} :", BenchId(a_cobj), a_cobj->data.numConstructed);
			a_cobj->requiredItems.ForEachContainerObject([&](RE::ContainerObject& a_entry) {
				if (a_entry.obj) {
					constexpr std::array kFamilies{ "métal"sv, "cuir"sv, "gemme"sv, "autre"sv };
					out += fmt::format(" {} x{} ({})", Describe(a_entry.obj), a_entry.count,
						kFamilies[std::to_underlying(a_mats.FamilyOf(a_entry.obj))]);
				}
				return RE::BSContainer::ForEachResult::kContinue;
			});
			return out;
		}

		bool IsPlayable(const RE::TESBoundObject* a_obj)
		{
			if (const auto armo = a_obj->As<RE::TESObjectARMO>()) {
				return (armo->formFlags & RE::TESObjectARMO::RecordFlags::kNonPlayable) == 0;
			}
			if (const auto weap = a_obj->As<RE::TESObjectWEAP>()) {
				return !weap->weaponData.flags.any(RE::TESObjectWEAP::Data::Flag::kNonPlayable) && !weap->IsBound();
			}
			return false;
		}

		const RE::TESBoundObject* TemplateOf(const RE::TESBoundObject* a_obj)
		{
			if (const auto armo = a_obj->As<RE::TESObjectARMO>()) {
				return armo->templateArmor;
			}
			if (const auto weap = a_obj->As<RE::TESObjectWEAP>()) {
				return weap->templateWeapon;
			}
			return nullptr;
		}

		const RE::BGSConstructibleObject* DirectOrTemplate(const RE::TESBoundObject* a_item, bool* a_viaTemplate = nullptr)
		{
			const auto* source = a_item;
			for (int depth = 0; source && depth < 4; ++depth, source = TemplateOf(source)) {
				if (const auto it = g_direct.find(source); it != g_direct.end()) {
					if (a_viaTemplate) {
						*a_viaTemplate = source != a_item;
					}
					return it->second.cobj;
				}
			}
			return nullptr;
		}

		// Materials of one unit of what a_recipe creates. Coins are dropped; equipment consumed by the recipe
		// (upgrades, recolors) is replaced by its own materials.
		void Expand(const RE::BGSConstructibleObject* a_recipe, float a_multiplier, int a_depth, std::vector<Ingredient>& a_out)
		{
			const float produced = std::max<float>(a_recipe->data.numConstructed, 1.0f);
			a_recipe->requiredItems.ForEachContainerObject([&](RE::ContainerObject& a_entry) {
				const auto obj = a_entry.obj;
				if (!obj || a_entry.count <= 0 || obj->GetFormID() == kGold) {
					return RE::BSContainer::ForEachResult::kContinue;
				}
				const float count = a_entry.count * a_multiplier / produced;
				if (obj->IsArmor() || obj->IsWeapon()) {
					if (const auto inner = a_depth < 3 ? DirectOrTemplate(obj) : nullptr; inner && inner != a_recipe) {
						Expand(inner, count, a_depth + 1, a_out);
					}
					return RE::BSContainer::ForEachResult::kContinue;
				}
				if (const auto it = std::ranges::find(a_out, obj, &Ingredient::obj); it != a_out.end()) {
					it->count += count;
				} else {
					a_out.push_back({ obj, count, g_mats.FamilyOf(obj) });
				}
				return RE::BSContainer::ForEachResult::kContinue;
			});
		}

		// A recipe that gives nothing back (coins only, or equipment without a recipe) is no reference.
		bool GivesMaterials(const RE::BGSConstructibleObject* a_recipe)
		{
			std::vector<Ingredient> ingredients;
			Expand(a_recipe, 1.0f, 0, ingredients);
			return !ingredients.empty();
		}

		// The ingot, leather or gem a recipe is mostly made of, by gold value; nullptr when it has none.
		const RE::TESForm* MainMaterial(const RE::BGSConstructibleObject* a_recipe)
		{
			std::vector<Ingredient> ingredients;
			Expand(a_recipe, 1.0f, 0, ingredients);
			const RE::TESForm* main = nullptr;
			float              best = 0.0f;
			for (const auto& ingredient : ingredients) {
				const float value = ingredient.count * std::max(ingredient.obj->GetGoldValue(), 1);
				if (ingredient.family != Family::kOther && value > best) {
					best = value;
					main = ingredient.obj;
				}
			}
			return main;
		}

		// Clothing is recycled at the tanning rack into linen or leather (Kevin, 2026-10-02): a mod recipe may stand in
		// for a whole part of the body only when it is made of the wanted material and nothing but linen / leather.
		constexpr RE::FormID kLinen = 0x00034CD6;  // Linen Wrap, Skyrim.esm

		bool IsClothRecipe(const RE::BGSConstructibleObject* a_recipe, bool a_wantLinen)
		{
			std::vector<Ingredient> ingredients;
			Expand(a_recipe, 1.0f, 0, ingredients);
			const auto isLinen = [](const Ingredient& a_i) { return a_i.obj->GetFormID() == kLinen; };
			const bool onlyCloth = std::ranges::all_of(ingredients, [&](const auto& i) { return isLinen(i) || i.family == Family::kLeather; });
			const bool hasLinen = std::ranges::any_of(ingredients, isLinen);
			return !ingredients.empty() && onlyCloth && (a_wantLinen ? hasLinen : !hasLinen);
		}

		// Jewelry stands in for jewelry only when made of metal or gems (some mods flag hats as jewelry).
		bool IsJewelryRecipe(const RE::BGSConstructibleObject* a_recipe)
		{
			std::vector<Ingredient> ingredients;
			Expand(a_recipe, 1.0f, 0, ingredients);
			return std::ranges::any_of(ingredients, [](const auto& i) { return i.family == Family::kMetal || i.family == Family::kGem; });
		}

		// Fallback for items without any recipe: the cheapest recipe of an item of the same kind and material.
		const RE::BGSKeyword* MaterialKeyword(const RE::TESBoundObject* a_item)
		{
			const auto form = a_item->As<RE::BGSKeywordForm>();
			if (!form) {
				return nullptr;
			}
			for (const auto keyword : form->GetKeywords()) {
				// "Materiel" too: Dawnguard spells some of its keywords that way.
				if (keyword && std::string_view{ keyword->GetFormEditorID() }.contains("Materi"sv)) {
					return keyword;
				}
			}
			return nullptr;
		}

		std::uint32_t ShapeOf(const RE::TESBoundObject* a_item)
		{
			if (const auto weap = a_item->As<RE::TESObjectWEAP>()) {
				return 0x10000 | std::to_underlying(weap->GetWeaponType());
			}
			const auto armo = a_item->As<RE::TESObjectARMO>();
			if (!armo) {
				return 0;
			}
			using Slot = RE::BGSBipedObjectForm::BipedObjectSlot;
			for (const auto slot : { Slot::kBody, Slot::kHead, Slot::kHands, Slot::kFeet, Slot::kShield, Slot::kAmulet, Slot::kRing, Slot::kCirclet }) {
				if (armo->HasPartOf(slot)) {
					return std::to_underlying(slot);
				}
			}
			return armo->GetSlotMask().underlying();
		}

		// Clothing and jewelry carry no material keyword: they are matched on where they are worn.
		enum class Wear : std::uint32_t
		{
			kBody,
			kHead,
			kHands,
			kFeet,
			kOther,
			kAmulet,
			kRing,
			kCirclet,
			kTotal
		};

		bool IsJewelry(const RE::TESObjectARMO* a_armor)
		{
			return a_armor->HasKeywordString("ArmorJewelry"sv);
		}

		Wear WearOf(const RE::TESObjectARMO* a_armor)
		{
			using Slot = RE::BGSBipedObjectForm::BipedObjectSlot;
			auto has = [&](std::initializer_list<Slot> a_slots) {
				return std::ranges::any_of(a_slots, [&](Slot a_slot) { return a_armor->HasPartOf(a_slot); });
			};
			if (IsJewelry(a_armor)) {
				return has({ Slot::kRing }) ? Wear::kRing : has({ Slot::kAmulet }) ? Wear::kAmulet : has({ Slot::kCirclet }) ? Wear::kCirclet : Wear::kOther;
			}
			if (has({ Slot::kBody })) {
				return Wear::kBody;
			}
			if (has({ Slot::kHead, Slot::kHair, Slot::kLongHair, Slot::kCirclet })) {
				return Wear::kHead;
			}
			if (has({ Slot::kHands, Slot::kForearms })) {
				return Wear::kHands;
			}
			if (has({ Slot::kFeet, Slot::kCalves })) {
				return Wear::kFeet;
			}
			return Wear::kOther;
		}

		// Default clothing recipes, used when no mod adds a clothing recipe for that part of the body
		// (decided with Kevin on 2026-10-02). Made at the tanning rack.
		constexpr RE::FormID kLeatherStrips = 0x000800E4;  // Skyrim.esm

		std::array<const RE::BGSConstructibleObject*, std::to_underlying(Wear::kTotal)> g_defaultClothing{};

		bool WantsLinen(Wear a_wear)
		{
			return !Settings::Get().clothLeather && (a_wear == Wear::kBody || a_wear == Wear::kHead || a_wear == Wear::kOther);
		}

		void CreateDefaultClothing(RE::BGSKeyword* a_tanningRack)
		{
			const auto linen = RE::TESForm::LookupByID<RE::TESObjectMISC>(kLinen);
			const auto strips = RE::TESForm::LookupByID<RE::TESObjectMISC>(kLeatherStrips);
			if (!linen || !strips) {
				SKSE::log::error("Lin ou bandes de cuir introuvables : pas de recettes de vêtements par défaut");
				return;
			}
			const auto factory = RE::IFormFactory::GetConcreteFormFactoryByType<RE::BGSConstructibleObject>();
			auto make = [&](RE::TESObjectMISC* a_material, std::int32_t a_count) {
				const auto recipe = factory->Create();
				recipe->createdItem = a_material;  // only read for the Smithing experience
				recipe->benchKeyword = a_tanningRack;
				recipe->data.numConstructed = 1;
				recipe->requiredItems.AddObjectToContainer(a_material, a_count, nullptr);
				return recipe;
			};
			const auto cloth = Settings::Get().clothLeather ? strips : linen;
			g_defaultClothing[std::to_underlying(Wear::kBody)] = make(cloth, 3);
			g_defaultClothing[std::to_underlying(Wear::kHead)] = make(cloth, 1);
			g_defaultClothing[std::to_underlying(Wear::kHands)] = make(strips, 1);
			g_defaultClothing[std::to_underlying(Wear::kFeet)] = make(strips, 1);
			g_defaultClothing[std::to_underlying(Wear::kOther)] = make(cloth, 1);
		}

		struct FallbackKey
		{
			bool                  weapon;
			const RE::BGSKeyword* material;
			std::uint32_t         shape;  // 0 = any shape
			bool                  operator==(const FallbackKey&) const = default;
		};

		struct FallbackKeyHash
		{
			std::size_t operator()(const FallbackKey& a_key) const noexcept
			{
				return std::hash<const void*>{}(a_key.material) ^ (static_cast<std::size_t>(a_key.shape) << 1) ^ static_cast<std::size_t>(a_key.weapon);
			}
		};

		// Miscellaneous items (dishes, tools, baskets...) have no recipe: SKSE/Plugins/KroosRecycleItems/Clutter.json
		// maps words of their 3D model's file name to a material.
		struct ClutterRule
		{
			std::vector<std::string>          words;
			std::int32_t                      lot{ 1 };
			Station                           station{ Station::kSmelter };
			const RE::BGSConstructibleObject* recipe{ nullptr };
		};

		RE::TESForm* LookupForm(const std::string& a_reference)
		{
			const auto bar = a_reference.find('|');
			if (bar == std::string::npos) {
				return nullptr;
			}
			const auto dataHandler = RE::TESDataHandler::GetSingleton();
			const auto id = static_cast<RE::FormID>(std::stoul(a_reference.substr(bar + 1), nullptr, 16));
			return dataHandler->LookupForm(id, a_reference.substr(0, bar));
		}

		std::vector<ClutterRule> LoadClutterRules(RE::BGSKeyword* a_smelter, RE::BGSKeyword* a_tanningRack)
		{
			std::vector<ClutterRule> rules;
			std::ifstream            file{ "Data/SKSE/Plugins/KroosRecycleItems/Clutter.json" };
			if (!file) {
				SKSE::log::warn("Clutter.json introuvable : pas d'objets divers recyclables");
				return rules;
			}
			try {
				const auto json = nlohmann::json::parse(file, nullptr, true, true);
				const auto factory = RE::IFormFactory::GetConcreteFormFactoryByType<RE::BGSConstructibleObject>();
				for (const auto& entry : json.at("rules")) {
					// "materialIron": what the rule gives instead when the MCM says simple plates give iron.
					const auto id = (Settings::Get().plateIron && entry.contains("materialIron") ? entry.at("materialIron") : entry.at("material")).get<std::string>();
					const auto material = LookupForm(id);
					const auto bound = material ? material->As<RE::TESBoundObject>() : nullptr;
					if (!bound) {
						SKSE::log::warn("Clutter.json : matériau introuvable {}", id);
						continue;
					}
					ClutterRule rule;
					for (const auto& word : entry.at("words")) {
						rule.words.push_back(word.get<std::string>());
					}
					rule.lot = std::max(entry.value("lot", 1), 1);
					rule.station = entry.value("station", "smelter"s) == "tanningRack" ? Station::kTanningRack : Station::kSmelter;
					const auto recipe = factory->Create();
					recipe->createdItem = bound;  // only read for the Smithing experience
					recipe->benchKeyword = rule.station == Station::kTanningRack ? a_tanningRack : a_smelter;
					recipe->data.numConstructed = 1;
					recipe->requiredItems.AddObjectToContainer(bound, std::max(entry.value("count", 1), 1), nullptr);
					rule.recipe = recipe;
					rules.push_back(std::move(rule));
				}
			} catch (const std::exception& e) {
				SKSE::log::error("Clutter.json illisible : {}", e.what());
			}
			return rules;
		}

		struct FileStats
		{
			std::uint32_t items{ 0 };
			std::uint32_t direct{ 0 };
			std::uint32_t viaTemplate{ 0 };
			std::uint32_t fallback{ 0 };
			std::uint32_t clothing{ 0 };
			std::uint32_t noRecipe{ 0 };
		};
	}

	void Run()
	{
		const auto dataHandler = RE::TESDataHandler::GetSingleton();
		if (!dataHandler) {
			SKSE::log::error("TESDataHandler introuvable, scan annulé");
			return;
		}
		const auto started = std::chrono::steady_clock::now();
		g_items.clear();
		auto&      cobjs = dataHandler->GetFormArray<RE::BGSConstructibleObject>();

		// 1. Material families, learned from what smelters and tanning racks produce. Hides are what tanning
		//    racks consume to make leather. Some mods smelt coins or soul gem fragments: not ingots.
		auto&                           mats = g_mats;
		std::vector<const RE::TESForm*> excluded;
		mats = {};
		for (const auto cobj : cobjs) {
			if (!cobj || !cobj->createdItem || !cobj->createdItem->Is(RE::FormType::Misc)) {
				continue;
			}
			const auto bench = BenchId(cobj);
			const auto created = cobj->createdItem->As<RE::TESObjectMISC>();
			if (bench == "CraftingSmelter"sv) {
				if (created->GetFormID() == kGold || created->HasKeywordString("VendorItemSoulGem"sv) || IsSoulGemFragment(created)) {
					if (std::ranges::find(excluded, created) == excluded.end()) {
						excluded.push_back(created);
					}
					continue;
				}
				mats.metal.insert(created);
			} else if (bench == "CraftingTanningRack"sv) {
				mats.leather.insert(created);
			}
		}
		for (const auto cobj : cobjs) {
			// Only single-ingredient leather recipes (hide -> leather): mods add dyed leather made from leather + charcoal, etc.
			if (cobj && cobj->createdItem && mats.leather.contains(cobj->createdItem) && BenchId(cobj) == "CraftingTanningRack"sv &&
				cobj->requiredItems.numContainerObjects == 1) {
				cobj->requiredItems.ForEachContainerObject([&](RE::ContainerObject& a_entry) {
					if (a_entry.obj && a_entry.obj->Is(RE::FormType::Misc) && !mats.leather.contains(a_entry.obj)) {
						mats.hides.insert(a_entry.obj);
					}
					return RE::BSContainer::ForEachResult::kContinue;
				});
			}
		}

		// 2. Cheapest creation recipe per armor / weapon; histogram of every bench that makes equipment.
		auto&                                recipes = g_direct;
		std::map<std::string, std::uint32_t> benchHistogram;
		recipes.clear();
		for (const auto cobj : cobjs) {
			if (!cobj || !cobj->createdItem) {
				continue;
			}
			const auto created = cobj->createdItem;
			if (!created->Is(RE::FormType::Armor) && !created->Is(RE::FormType::Weapon)) {
				continue;
			}
			const auto bench = BenchId(cobj);
			++benchHistogram[std::string{ bench }];
			if (!IsCreationBench(bench)) {
				continue;
			}
			std::int32_t total = 0;
			cobj->requiredItems.ForEachContainerObject([&](RE::ContainerObject& a_entry) {
				total += a_entry.count;
				return RE::BSContainer::ForEachResult::kContinue;
			});
			auto& best = recipes[created];
			if (!best.cobj || total < best.total) {
				best = { cobj, total };
			}
		}

		// Fallback index, built from items that have a recipe of their own. A material keyword can lie (vanilla
		// tags the Hunting Bow as steel, a mod may tag an ebony bow as steel; reported on 2026-10-09): a recipe only
		// stands in for its keyword when made of the material most recipes of that keyword are made of.
		std::unordered_map<const RE::BGSKeyword*, std::unordered_map<const RE::TESForm*, std::uint32_t>> votes;
		for (const auto& [created, recipe] : recipes) {
			const auto item = created->As<RE::TESBoundObject>();
			const auto material = item ? MaterialKeyword(item) : nullptr;
			const auto main = material ? MainMaterial(recipe.cobj) : nullptr;
			if (main) {
				++votes[material][main];
			}
		}
		std::unordered_map<const RE::BGSKeyword*, const RE::TESForm*> usualMaterial;
		for (const auto& [keyword, counts] : votes) {
			usualMaterial[keyword] = std::ranges::max_element(counts, {}, [](const auto& a_pair) { return a_pair.second; })->first;
		}
		std::uint32_t                                            unusualRecipes = 0;
		std::unordered_map<FallbackKey, Recipe, FallbackKeyHash> fallbacks;
		for (const auto& [created, recipe] : recipes) {
			const auto item = created->As<RE::TESBoundObject>();
			const auto material = item ? MaterialKeyword(item) : nullptr;
			if (!material || !GivesMaterials(recipe.cobj)) {
				continue;
			}
			if (MainMaterial(recipe.cobj) != usualMaterial[material]) {
				++unusualRecipes;
				continue;
			}
			for (const auto key : { FallbackKey{ item->IsWeapon(), material, ShapeOf(item) }, FallbackKey{ item->IsWeapon(), material, 0 } }) {
				auto& best = fallbacks[key];
				if (!best.cobj || recipe.total < best.total) {
					best = recipe;
				}
			}
		}
		std::uint32_t fallbackExact = 0, fallbackMaterial = 0;

		// Clothing / jewelry index from the recipes the load order provides (Common Clothing Expanded...).
		std::array<Recipe, std::to_underlying(Wear::kTotal)> wornRecipes{};
		RE::BGSKeyword*                                      tanningRack = nullptr;
		for (const auto& [created, recipe] : recipes) {
			if (!tanningRack && BenchId(recipe.cobj) == "CraftingTanningRack"sv) {
				tanningRack = recipe.cobj->benchKeyword;
			}
			const auto armor = created->As<RE::TESObjectARMO>();
			if (!armor || (!armor->IsClothing() && !IsJewelry(armor))) {
				continue;
			}
			const auto wear = WearOf(armor);
			if (IsJewelry(armor) ? !IsJewelryRecipe(recipe.cobj) : !IsClothRecipe(recipe.cobj, WantsLinen(wear))) {
				continue;
			}
			auto& best = wornRecipes[std::to_underlying(wear)];
			if (!best.cobj || recipe.total < best.total) {
				best = recipe;
			}
		}
		if (!g_defaultClothing[0]) {
			CreateDefaultClothing(tanningRack);
		}
		// A mod recipe stands in for a part of the body only if it gives at least as much as the default one
		// (a simple linen top must not make every robe worth one piece of linen; Kevin, 2026-10-03).
		auto amount = [](const RE::BGSConstructibleObject* a_recipe) {
			std::vector<Ingredient> ingredients;
			Expand(a_recipe, 1.0f, 0, ingredients);
			float total = 0.0f;
			for (const auto& ingredient : ingredients) {
				total += ingredient.count;
			}
			return total;
		};
		for (std::size_t i = 0; i < std::to_underlying(Wear::kAmulet); ++i) {
			if (wornRecipes[i].cobj && g_defaultClothing[i] && amount(wornRecipes[i].cobj) < amount(g_defaultClothing[i])) {
				wornRecipes[i] = {};
			}
		}
		std::vector<std::string> wornReport;
		for (std::size_t i = 0; i < wornRecipes.size(); ++i) {
			constexpr std::array kWear{ "corps"sv, "tête"sv, "mains"sv, "pieds"sv, "autre"sv, "amulette"sv, "anneau"sv, "diadème"sv };
			wornReport.push_back(fmt::format("{} : {}", kWear[i],
				wornRecipes[i].cobj ? fmt::format("{} {}", Describe(wornRecipes[i].cobj->createdItem), DescribeRecipe(wornRecipes[i].cobj, mats)) : "défaut"s));
		}
		std::uint32_t clothingFromMods = 0, clothingDefault = 0, staffFallbacks = 0;

		// Staves without a recipe of their own: the cheapest staff enchanter recipe (heart stones).
		const RE::BGSConstructibleObject* staffFallback = nullptr;
		std::int32_t                      staffFallbackTotal = 0;
		for (const auto& [created, recipe] : recipes) {
			if (BenchId(recipe.cobj) == "DLC2StaffEnchanter"sv && GivesMaterials(recipe.cobj) &&
				(!staffFallback || recipe.total < staffFallbackTotal)) {
				staffFallback = recipe.cobj;
				staffFallbackTotal = recipe.total;
			}
		}

		// 3. Classify every playable armor and weapon.
		std::map<std::string, FileStats, std::less<>> perFile;
		std::array<std::uint32_t, 5>                   perStation{};
		std::uint32_t                                  jewelryGem = 0, otherTanning = 0, otherSmelter = 0;
		std::vector<std::string>                       ties, noMaterial, noRecipeSample;
		std::uint32_t                                  total = 0;

		auto classify = [&](const RE::TESBoundObject* a_item) {
			if (!IsPlayable(a_item)) {
				return;
			}
			const char* name = a_item->GetName();
			if (!name || !*name) {
				return;
			}
			++total;
			auto& stats = perFile[std::string{ FileOf(a_item) }];
			++stats.items;

			bool                              viaTemplate = false;
			bool                              borrowed = false;
			const RE::BGSConstructibleObject* recipe = DirectOrTemplate(a_item, &viaTemplate);
			if (recipe && !GivesMaterials(recipe)) {
				recipe = nullptr;
			}
			if (recipe) {
				++(viaTemplate ? stats.viaTemplate : stats.direct);
			}
			if (const auto material = recipe ? nullptr : MaterialKeyword(a_item)) {
				const auto exact = fallbacks.find({ a_item->IsWeapon(), material, ShapeOf(a_item) });
				const auto loose = fallbacks.find({ a_item->IsWeapon(), material, 0 });
				if (exact != fallbacks.end()) {
					recipe = exact->second.cobj;
					++fallbackExact;
				} else if (loose != fallbacks.end()) {
					recipe = loose->second.cobj;
					++fallbackMaterial;
				}
				if (recipe) {
					++stats.fallback;
					borrowed = true;
				}
			}
			if (const auto armor = a_item->As<RE::TESObjectARMO>(); !recipe && armor && (armor->IsClothing() || IsJewelry(armor))) {
				const auto wear = std::to_underlying(WearOf(armor));
				if (wornRecipes[wear].cobj) {
					recipe = wornRecipes[wear].cobj;
					++clothingFromMods;
				} else if (!IsJewelry(armor)) {
					recipe = g_defaultClothing[wear];
					++clothingDefault;
				}
				if (recipe) {
					++stats.clothing;
				}
			}
			if (const auto weap = a_item->As<RE::TESObjectWEAP>();
				!recipe && weap && weap->GetWeaponType() == RE::WEAPON_TYPE::kStaff && staffFallback) {
				recipe = staffFallback;
				borrowed = true;
				++staffFallbacks;
			}
			if (recipe && BenchId(recipe) == "DLC2StaffEnchanter"sv) {
				// Staves go back to the table that made them; off when the player chose so in the FOMOD.
				if (!Settings::Get().staves) {
					return;
				}
				++perStation[std::to_underlying(Station::kStaffEnchanter)];
				g_items[a_item] = { Station::kStaffEnchanter, recipe, borrowed };
				return;
			}
			if (!recipe) {
				++stats.noRecipe;
				if (noRecipeSample.size() < 60) {
					noRecipeSample.push_back(fmt::format("{} — {}", Describe(a_item), FileOf(a_item)));
				}
				return;
			}

			// Main material by gold value: "1 ingot + 1 leather strip" is a metal item, not a tie.
			float                   metal = 0.0f, leather = 0.0f;
			bool                    gem = false;
			std::vector<Ingredient> ingredients;
			Expand(recipe, 1.0f, 0, ingredients);
			for (const auto& ingredient : ingredients) {
				const auto value = ingredient.count * std::max(ingredient.obj->GetGoldValue(), 1);
				switch (ingredient.family) {
				case Family::kMetal:
					metal += value;
					break;
				case Family::kLeather:
					leather += value;
					break;
				case Family::kGem:
					gem = true;
					break;
				default:
					break;
				}
			}

			const auto armo = a_item->As<RE::TESObjectARMO>();
			if (gem && armo && armo->HasKeywordString("ArmorJewelry"sv)) {
				++jewelryGem;
				++perStation[std::to_underlying(Station::kSmelter)];
				g_items[a_item] = { Station::kSmelter, recipe, borrowed };
				return;
			}
			Station station = Station::kNone;
			if (metal > leather) {
				station = Station::kSmelter;
			} else if (leather > metal) {
				station = Station::kTanningRack;
			} else if (metal > 0) {
				station = Station::kBoth;
				ties.push_back(fmt::format("{} — {} — {}", Describe(a_item), FileOf(a_item), DescribeRecipe(recipe, mats)));
			} else {
				// Neither ingot nor leather: cloth made at a tanning rack stays there, rare materials go to the smelter.
				const bool cloth = BenchId(recipe) == "CraftingTanningRack"sv;
				station = cloth ? Station::kTanningRack : Station::kSmelter;
				++(cloth ? otherTanning : otherSmelter);
				noMaterial.push_back(fmt::format("{} — {} — {}", Describe(a_item), FileOf(a_item), DescribeRecipe(recipe, mats)));
			}
			++perStation[std::to_underlying(station)];
			g_items[a_item] = { station, recipe, borrowed };
		};
		for (const auto armo : dataHandler->GetFormArray<RE::TESObjectARMO>()) {
			if (armo) {
				classify(armo);
			}
		}
		for (const auto weap : dataHandler->GetFormArray<RE::TESObjectWEAP>()) {
			if (weap) {
				classify(weap);
			}
		}

		// Miscellaneous items. Anything already used by a smelter or tanning rack recipe (ores, Dwemer scrap, hides)
		// keeps its vanilla use; materials themselves are never recycled.
		RE::BGSKeyword*                                smelter = nullptr;
		std::unordered_set<const RE::TESBoundObject*> stationInputs;
		for (const auto cobj : cobjs) {
			if (!cobj) {
				continue;
			}
			const auto bench = BenchId(cobj);
			if (bench == "CraftingSmelter"sv || bench == "CraftingTanningRack"sv) {
				if (!smelter && bench == "CraftingSmelter"sv) {
					smelter = cobj->benchKeyword;
				}
				cobj->requiredItems.ForEachContainerObject([&](RE::ContainerObject& a_entry) {
					stationInputs.insert(a_entry.obj);
					return RE::BSContainer::ForEachResult::kContinue;
				});
			}
		}
		const auto                 clutterRules = LoadClutterRules(smelter, tanningRack);
		std::vector<std::uint32_t> clutterPerRule(clutterRules.size());
		g_lots.clear();
		for (const auto misc : dataHandler->GetFormArray<RE::TESObjectMISC>()) {
			if (!misc || !misc->GetName() || !*misc->GetName() || stationInputs.contains(misc) || mats.FamilyOf(misc) != Family::kOther ||
				misc->GetFormID() == kGold) {
				continue;
			}
			std::string model = misc->GetModel() ? misc->GetModel() : "";
			std::ranges::transform(model, model.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			const auto slash = model.find_last_of("\\/");
			const std::string_view file = slash == std::string::npos ? std::string_view{ model } : std::string_view{ model }.substr(slash + 1);
			for (std::size_t i = 0; i < clutterRules.size(); ++i) {
				const auto& rule = clutterRules[i];
				if (std::ranges::any_of(rule.words, [&](const auto& w) { return file.contains(w); })) {
					g_items[misc] = { rule.station, rule.recipe };
					g_lots[misc] = rule.lot;
					++clutterPerRule[i];
					break;
				}
			}
		}

		// 4. Report.
		const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started);
		SKSE::log::info("===== Scan des recettes ({} ms) =====", elapsed.count());
		SKSE::log::info("Recettes (COBJ) : {} | métal : {} | cuir : {} | peaux : {}", static_cast<std::uint32_t>(cobjs.size()), mats.metal.size(), mats.leather.size(), mats.hides.size());
		SKSE::log::info("Armes/armures jouables : {}", total);
		SKSE::log::info("  fonderie : {} (dont bijoux à gemme : {}, matières rares : {})", perStation[0], jewelryGem, otherSmelter);
		SKSE::log::info("  chevalet : {} (dont lin/tissu : {})", perStation[1], otherTanning);
		SKSE::log::info("  égalité (fonderie + chevalet) : {}", perStation[2]);
		SKSE::log::info("  table d'enchantement de bâtons : {} (dont {} par recette de secours)", perStation[4], staffFallbacks);
		std::uint32_t noRecipe = 0;
		for (const auto& [file, stats] : perFile) {
			noRecipe += stats.noRecipe;
		}
		SKSE::log::info("  recette de secours (même matériau) : {} même forme + {} matériau seul", fallbackExact, fallbackMaterial);
		SKSE::log::info("  recettes écartées comme secours (matériau inhabituel pour leur mot-clé) : {}", unusualRecipes);
		SKSE::log::info("  vêtements / bijoux par emplacement : {} recette d'un mod + {} recette par défaut", clothingFromMods, clothingDefault);
		SKSE::log::info("  sans recette : {}", noRecipe);
		SKSE::log::info("Objets divers recyclables : {} ({} règles)", g_lots.size(), clutterRules.size());
		for (std::size_t i = 0; i < clutterRules.size(); ++i) {
			SKSE::log::info("  règle {} ({}…) : {} objets", i + 1, clutterRules[i].words.front(), clutterPerRule[i]);
		}

		SKSE::log::info("----- Vêtements / bijoux : recette de référence par emplacement -----");
		for (const auto& line : wornReport) {
			SKSE::log::info("  {}", line);
		}
		SKSE::log::info("----- Établis qui fabriquent des armes/armures (recettes) -----");
		for (const auto& [bench, count] : benchHistogram) {
			SKSE::log::info("  {} : {}{}", bench, count, IsCreationBench(bench) ? "  <- utilisé" : "");
		}

		SKSE::log::info("----- Matériaux métal (produits à la fonderie) -----");
		for (const auto form : mats.metal) {
			SKSE::log::info("  {} — {}", Describe(form), FileOf(form));
		}
		SKSE::log::info("----- Matériaux cuir (produits au chevalet) -----");
		for (const auto form : mats.leather) {
			SKSE::log::info("  {} — {}", Describe(form), FileOf(form));
		}
		SKSE::log::info("----- Peaux (transformées en cuir au chevalet) -----");
		for (const auto form : mats.hides) {
			SKSE::log::info("  {} — {}", Describe(form), FileOf(form));
		}
		SKSE::log::info("----- Exclus des lingots (produits à la fonderie) -----");
		for (const auto form : excluded) {
			SKSE::log::info("  {} — {}", Describe(form), FileOf(form));
		}

		SKSE::log::info("----- Par fichier : objets | recette directe | via modèle | secours | emplacement | sans recette -----");
		for (const auto& [file, stats] : perFile) {
			SKSE::log::info("  {} : {} | {} | {} | {} | {} | {}", file, stats.items, stats.direct, stats.viaTemplate, stats.fallback, stats.clothing, stats.noRecipe);
		}

		SKSE::log::info("----- Égalités ({}) -----", ties.size());
		for (const auto& line : ties) {
			SKSE::log::info("  {}", line);
		}
		SKSE::log::info("----- Ni lingot ni cuir : lin/tissu -> chevalet, matières rares -> fonderie ({}) -----", noMaterial.size());
		for (const auto& line : noMaterial) {
			SKSE::log::info("  {}", line);
		}
		SKSE::log::info("----- Sans recette : {} premiers -----", noRecipeSample.size());
		for (const auto& line : noRecipeSample) {
			SKSE::log::info("  {}", line);
		}

		SKSE::log::info("----- Contrôles -----");
		for (const auto formID : { 0x00012E49u, 0x0001397Eu }) {  // expected: iron armor, iron dagger
			const auto form = RE::TESForm::LookupByID<RE::TESBoundObject>(formID);
			if (!form) {
				SKSE::log::info("  {:08X} : introuvable", formID);
				continue;
			}
			const auto it = recipes.find(form);
			SKSE::log::info("  {} : {}", Describe(form), it != recipes.end() ? DescribeRecipe(it->second.cobj, mats) : "pas de recette"s);
		}
		SKSE::log::info("===== Fin du scan =====");
	}

	std::int32_t LotSize(const RE::TESBoundObject* a_item)
	{
		const auto it = g_lots.find(a_item);
		return it != g_lots.end() ? it->second : 1;
	}

	Station StationOf(const RE::TESBoundObject* a_item)
	{
		const auto it = g_items.find(a_item);
		return it != g_items.end() ? it->second.station : Station::kNone;
	}

	const RE::BGSConstructibleObject* ReferenceRecipe(const RE::TESBoundObject* a_item)
	{
		const auto it = g_items.find(a_item);
		return it != g_items.end() ? it->second.recipe : nullptr;
	}

	bool IsBorrowed(const RE::TESBoundObject* a_item)
	{
		const auto it = g_items.find(a_item);
		return it != g_items.end() && it->second.borrowed;
	}

	Family FamilyOf(const RE::TESBoundObject* a_material)
	{
		return g_mats.FamilyOf(a_material);
	}

	std::vector<Ingredient> IngredientsOf(const RE::TESBoundObject* a_item)
	{
		std::vector<Ingredient> ingredients;
		if (const auto recipe = ReferenceRecipe(a_item)) {
			Expand(recipe, 1.0f, 0, ingredients);
		}
		return ingredients;
	}
}
