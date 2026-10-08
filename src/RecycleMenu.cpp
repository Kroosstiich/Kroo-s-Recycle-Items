#include "RecycleMenu.h"

#include "RecipeScan.h"
#include "Recycler.h"
#include "Settings.h"
#include "Text.h"

#include <unordered_map>

namespace KRI::RecycleMenu
{
	namespace
	{
		using ItemEntry = RE::CraftingSubMenus::ConstructibleObjectMenu::ItemEntry;
		using Station = RecipeScan::Station;

		enum class Category
		{
			kWeapons,
			kArmor,
			kJewelry,
			kClothing,
			kMisc,
			kTotal
		};

		// Keyword EditorIDs referenced by SKSE\Plugins\CraftingCategories\KroosRecycleItems.json.
		constexpr std::array<const char*, std::to_underlying(Category::kTotal)> kCategoryKeywords{
			"KRI_CatWeapons", "KRI_CatArmor", "KRI_CatJewelry", "KRI_CatClothing", "KRI_CatMisc"
		};

		// CraftingCategories sorts a recipe by the keywords of its created item. Our recipes temporarily create
		// a category token while the list is built, then the real item again (item card, 3D preview).
		std::array<RE::TESObjectMISC*, std::to_underlying(Category::kTotal)> g_tokens{};

		struct RecipeKey
		{
			const RE::TESBoundObject* item;
			Station                   station;
			bool                      locked{ false };  // the player lacks the levels: shown greyed out
			bool                      operator==(const RecipeKey&) const = default;
		};

		struct RecipeKeyHash
		{
			std::size_t operator()(const RecipeKey& a_key) const noexcept
			{
				return std::hash<const void*>{}(a_key.item) ^ (std::to_underlying(a_key.station) << 1) ^ (static_cast<std::size_t>(a_key.locked) << 4);
			}
		};

		std::unordered_map<RecipeKey, RE::BGSConstructibleObject*, RecipeKeyHash> g_recipes;
		std::unordered_map<const RE::BGSConstructibleObject*, RecipeKey>          g_recipeKeys;

		Category CategoryOf(const RE::TESBoundObject* a_item)
		{
			if (a_item->Is(RE::FormType::Misc)) {
				return Category::kMisc;
			}
			const auto armo = a_item->As<RE::TESObjectARMO>();
			if (!armo) {
				return Category::kWeapons;
			}
			if (armo->HasKeywordString("ArmorJewelry"sv)) {
				return Category::kJewelry;
			}
			return armo->IsClothing() ? Category::kClothing : Category::kArmor;
		}

		Station BenchStation(const RE::TESFurniture* a_furniture)
		{
			if (a_furniture && a_furniture->HasKeywordString("CraftingSmelter"sv)) {
				return Station::kSmelter;
			}
			if (a_furniture && a_furniture->HasKeywordString("CraftingTanningRack"sv)) {
				return Station::kTanningRack;
			}
			if (a_furniture && a_furniture->HasKeywordString("DLC2StaffEnchanter"sv)) {
				return Station::kStaffEnchanter;
			}
			return Station::kNone;
		}

		RE::BGSConstructibleObject* RecipeFor(RE::TESBoundObject* a_item, Station a_bench, bool a_locked)
		{
			auto& recipe = g_recipes[{ a_item, a_bench, a_locked }];
			if (!recipe) {
				recipe = RE::IFormFactory::GetConcreteFormFactoryByType<RE::BGSConstructibleObject>()->Create();
				recipe->createdItem = a_item;
				recipe->data.numConstructed = 1;
				// Shown as the requirement; the craft action itself is intercepted (see CraftHooks).
				recipe->requiredItems.AddObjectToContainer(a_item, RecipeScan::LotSize(a_item), nullptr);
				if (a_locked) {
					// The category token is never in the inventory: the menu greys the entry out.
					recipe->requiredItems.AddObjectToContainer(g_tokens[std::to_underlying(CategoryOf(a_item))], 1, nullptr);
				}
				g_recipeKeys[recipe] = { a_item, a_bench, a_locked };
			}
			return recipe;
		}

		struct Added
		{
			std::uint32_t               index;
			RE::BGSConstructibleObject* recipe;
			RE::TESBoundObject*         item;
			std::string                 text;
		};

		// Recyclable items the player carries, minus worn copies; quest items are left out entirely.
		std::vector<Added> AppendEntries(Station a_bench, RE::BSTArray<ItemEntry>& a_entries)
		{
			std::vector<Added> added;
			const auto         player = RE::PlayerCharacter::GetSingleton();
			if (!player) {
				return added;
			}
			auto inventory = player->GetInventory([](RE::TESBoundObject& a_obj) {
				return a_obj.IsArmor() || a_obj.IsWeapon() || a_obj.Is(RE::FormType::Misc);
			});
			for (auto& [item, data] : inventory) {
				auto& [count, entry] = data;
				if (count <= 0 || !entry || entry->IsQuestObject()) {
					continue;
				}
				const auto station = RecipeScan::StationOf(item);
				if (station == Station::kNone || (station != Station::kBoth && station != a_bench)) {
					continue;
				}
				std::int32_t worn = 0;
				if (entry->extraLists) {
					for (const auto xList : *entry->extraLists) {
						if (xList && (xList->HasType<RE::ExtraWorn>() || xList->HasType<RE::ExtraWornLeft>())) {
							worn += xList->GetCount();
						}
					}
				}
				const auto available = count - worn;
				const auto lot = RecipeScan::LotSize(item);
				if (available < lot) {
					continue;
				}
				const auto recipe = RecipeFor(item, a_bench, !Recycler::MissingLevels(item).empty());
				const auto name = item->GetName();
				const auto text = lot > 1          ? Text::Format("$KRI_LotLabel", name, lot, available) :
				                  available > 1 ? fmt::format("{} ({})", name, available) :
				                                  std::string{ name };
				added.push_back({ a_entries.size(), recipe, item, text });
				recipe->createdItem = g_tokens[std::to_underlying(CategoryOf(item))];
				a_entries.push_back({ recipe, 0, 0 });
			}
			return added;
		}

		struct SetItemEntryData
		{
			static void thunk(RE::CraftingSubMenus::ConstructibleObjectMenu* a_menu, RE::BSTArray<ItemEntry>& a_entries)
			{
				const auto bench = BenchStation(a_menu ? a_menu->furniture : nullptr);
				if (bench == Station::kNone) {
					func(a_menu, a_entries);
					return;
				}

				Settings::Load();  // the MCM may have changed something since the last time
				const auto vanilla = a_entries.size();
				auto       added = AppendEntries(bench, a_entries);
				func(a_menu, a_entries);

				for (const auto& entry : added) {
					entry.recipe->createdItem = entry.item;
					RE::GFxValue object;
					if (a_menu->entryList.GetElement(entry.index, &object) && object.IsObject()) {
						static bool logged = false;
						if (!logged) {
							logged = true;
							std::string members;
							object.VisitMembers([&](const char* a_name, const RE::GFxValue&) { members += fmt::format(" {}", a_name); });
							SKSE::log::info("Membres d'une entrée de liste :{}", members);
						}
						object.SetMember("text", RE::GFxValue{ entry.text.c_str() });
					}
				}
				SKSE::log::info("Menu {} : {} recettes, {} objets à recycler",
					bench == Station::kSmelter ? "fonderie" : bench == Station::kTanningRack ? "chevalet" : "table des bâtons",
					vanilla, added.size());
			}

			static inline REL::Relocation<decltype(thunk)> func;
		};

		// "ChooseItem" (click or Activate on a list entry, index argument) and "CraftButtonPress" (button) are
		// the two ways the menu crafts; the SWF also calls "CraftSelectedItem", which the game never registers.
		// Ours ask for confirmation and recycle instead when the selected recipe is a recycling one.
		struct CraftHooks
		{
			inline static RE::FxDelegateHandler::CallbackFn* chooseItem = nullptr;
			inline static RE::FxDelegateHandler::CallbackFn* craftButton = nullptr;
			inline static RE::FxDelegateHandler::CallbackFn* setSelected = nullptr;

			// Replace the "requires: <item>" line with what recycling gives back.
			static void OnSetSelected(const RE::FxDelegateArgs& a_args)
			{
				if (setSelected) {
					setSelected(a_args);
				}
				ShowYieldLine(SubMenuOf(a_args.GetHandler()));
			}

			static void ShowYieldLine(RE::CraftingSubMenus::ConstructibleObjectMenu* a_menu)
			{
				const auto menu = a_menu;
				if (!menu || !menu->view || menu->currentIndex >= menu->recipes.size()) {
					return;
				}
				const auto it = g_recipeKeys.find(menu->recipes[menu->currentIndex].constructibleObject);
				if (it == g_recipeKeys.end()) {
					return;
				}
				// Not yet allowed (enchanted item, staff): show what is missing, greyed out, instead of the chances.
				const auto missing = Recycler::MissingLevels(it->second.item);
				const auto labels = missing.empty() ? Recycler::YieldLabels(it->second.item, it->second.station) : std::vector<std::string>{ missing };
				const auto   view = menu->view.get();
				RE::GFxValue ingredients;
				view->CreateArray(&ingredients);
				ingredients.SetArraySize(static_cast<std::uint32_t>(std::max<std::size_t>(labels.size(), 1)));
				for (std::uint32_t i = 0; i < std::max<std::size_t>(labels.size(), 1); ++i) {
					RE::GFxValue entry, name;
					view->CreateObject(&entry);
					// One material per line: the card is too narrow for three of them on one line.
					const auto label = labels.empty() ? Text::Get("$KRI_Nothing") : (i > 0 ? "\n"s : ""s) + labels[i];
					view->CreateString(&name, label.c_str());
					entry.SetMember("Name", name);
					entry.SetMember("RequiredCount", RE::GFxValue{ 1 });
					entry.SetMember("PlayerCount", RE::GFxValue{ missing.empty() ? 1 : 0 });
					ingredients.SetElement(i, entry);
				}
				RE::GFxValue title;
				view->CreateString(&title, Text::Get(missing.empty() ? "$KRI_YieldTitle" : "$KRI_ReqTitle").c_str());
				const std::array<RE::GFxValue, 3> args{ title, ingredients, RE::GFxValue{ false } };
				view->Invoke("Menu.UpdateIngredients", nullptr, args.data(), args.size());
			}

			// The handler is the submenu for its own callbacks and the CraftingMenu for the ones it registers.
			static RE::CraftingSubMenus::ConstructibleObjectMenu* SubMenuOf(RE::FxDelegateHandler* a_handler)
			{
				if (const auto subMenu = skyrim_cast<RE::CraftingSubMenus::ConstructibleObjectMenu*>(a_handler)) {
					return subMenu;
				}
				const auto menu = skyrim_cast<RE::CraftingMenu*>(a_handler);
				return menu ? skyrim_cast<RE::CraftingSubMenus::ConstructibleObjectMenu*>(menu->subMenu) : nullptr;
			}

			static bool TryRecycle(const RE::FxDelegateArgs& a_args, bool a_useArgument)
			{
				const auto menu = SubMenuOf(a_args.GetHandler());
				if (!menu) {
					SKSE::log::info("Fabrication : pas un menu de recettes");
					return false;
				}
				std::uint32_t index = menu->currentIndex;
				if (a_useArgument && a_args.GetArgCount() > 0 && a_args[0].IsNumber()) {
					index = static_cast<std::uint32_t>(a_args[0].GetNumber());
				}
				return RecycleAt(menu, index);
			}

			static bool RecycleAt(RE::CraftingSubMenus::ConstructibleObjectMenu* a_menu, std::uint32_t a_index)
			{
				if (a_index >= a_menu->recipes.size()) {
					SKSE::log::info("Fabrication : index {} hors liste ({})", a_index, a_menu->recipes.size());
					return false;
				}
				const auto it = g_recipeKeys.find(a_menu->recipes[a_index].constructibleObject);
				if (it == g_recipeKeys.end()) {
					return false;
				}
				SKSE::log::info("Recyclage demandé : {} (index {}, courant {})", it->second.item->GetName(), a_index, a_menu->currentIndex);
				Recycler::Confirm(const_cast<RE::TESBoundObject*>(it->second.item), it->second.station);
				return true;
			}

			static void OnChooseItem(const RE::FxDelegateArgs& a_args)
			{
				if (!TryRecycle(a_args, true) && chooseItem) {
					chooseItem(a_args);
				}
			}

			static void OnCraftButton(const RE::FxDelegateArgs& a_args)
			{
				if (!TryRecycle(a_args, false) && craftButton) {
					craftButton(a_args);
				}
			}
		};

		class ProcessorProxy : public RE::FxDelegateHandler::CallbackProcessor
		{
		public:
			explicit ProcessorProxy(RE::FxDelegateHandler::CallbackProcessor* a_inner) :
				_inner(a_inner)
			{}

			void Process(const RE::GString& a_methodName, RE::FxDelegateHandler::CallbackFn* a_method) override
			{
				const std::string_view name{ a_methodName.c_str() };
				if (name == "ChooseItem"sv) {
					CraftHooks::chooseItem = a_method;
					a_method = CraftHooks::OnChooseItem;
				} else if (name == "SetSelectedItem"sv) {
					CraftHooks::setSelected = a_method;
					a_method = CraftHooks::OnSetSelected;
				} else if (name == "CraftButtonPress"sv) {
					CraftHooks::craftButton = a_method;
					a_method = CraftHooks::OnCraftButton;
				}
				_inner->Process(a_methodName, a_method);
			}

		private:
			RE::FxDelegateHandler::CallbackProcessor* _inner;
		};

		template <class Menu>
		struct Accept
		{
			static void thunk(Menu* a_menu, RE::FxDelegateHandler::CallbackProcessor* a_processor)
			{
				ProcessorProxy proxy{ a_processor };
				func(a_menu, &proxy);
				SKSE::log::debug("Rappels interceptés ({}) : choix {}, bouton {}", typeid(Menu).name(), CraftHooks::chooseItem != nullptr,
					CraftHooks::craftButton != nullptr);
			}

			static inline REL::Relocation<decltype(thunk)> func;
		};

		// Safety net: the vanilla "create this item?" box must never craft a recycling recipe (it would destroy
		// the exact copy and recreate a plain one). Show our confirmation instead.
		struct CreationConfirmRun
		{
			static void thunk(RE::CraftingSubMenus::ConstructibleObjectMenu::CreationConfirmCallback* a_callback, std::uint8_t a_button)
			{
				const auto menu = a_callback->subMenu;
				if (menu && menu->currentIndex < menu->recipes.size() &&
					g_recipeKeys.contains(menu->recipes[menu->currentIndex].constructibleObject)) {
					SKSE::log::warn("Confirmation vanilla sur une recette de recyclage : bloquée");
					if (a_button == 0) {
						CraftHooks::RecycleAt(menu, menu->currentIndex);
					}
					return;
				}
				func(a_callback, a_button);
			}

			static inline REL::Relocation<decltype(thunk)> func;
		};
	}

	namespace
	{
		RE::CraftingSubMenus::ConstructibleObjectMenu* OpenSubMenu()
		{
			const auto ui = RE::UI::GetSingleton();
			const auto menu = ui ? ui->GetMenu<RE::CraftingMenu>() : nullptr;
			return menu ? skyrim_cast<RE::CraftingSubMenus::ConstructibleObjectMenu*>(menu->subMenu) : nullptr;
		}

		// CraftingCategories numbers categories in the order it meets them, so a rebuilt list (an emptied
		// category, a new one) can renumber them while the SWF keeps filtering on the old number: the items
		// of the open category vanish. Remember the open category by its label, then filter on its new number.
		// Only plain fields are read (getters such as selectedEntry are not reachable from C++).
		std::string OpenCategoryLabel(RE::GFxMovieView* a_view)
		{
			RE::GFxValue filter, entries;
			if (!a_view || !a_view->GetVariable(&filter, "Menu.InventoryLists._typeFilter._itemFilter") || !filter.IsNumber() ||
				!a_view->GetVariable(&entries, "Menu.InventoryLists.CategoriesList._entryList") || !entries.IsArray()) {
				SKSE::log::info("Catégorie ouverte : filtre ou liste illisible");
				return {};
			}
			for (std::uint32_t i = 0; i < entries.GetArraySize(); ++i) {
				RE::GFxValue entry, text, flag;
				if (entries.GetElement(i, &entry) && entry.GetMember("flag", &flag) && flag.IsNumber() &&
					flag.GetNumber() == filter.GetNumber() && entry.GetMember("text", &text) && text.IsString()) {
					SKSE::log::info("Catégorie ouverte : {} (filtre {})", text.GetString(), filter.GetNumber());
					return text.GetString();
				}
			}
			SKSE::log::info("Catégorie ouverte : aucune pour le filtre {}", filter.GetNumber());
			return {};
		}

		void ReopenCategory(RE::GFxMovieView* a_view, const std::string& a_label)
		{
			RE::GFxValue entries, filter, layout;
			if (a_label.empty() || !a_view || !a_view->GetVariable(&entries, "Menu.InventoryLists.CategoriesList._entryList") ||
				!entries.IsArray()) {
				return;
			}
			for (std::uint32_t i = 0; i < entries.GetArraySize(); ++i) {
				RE::GFxValue entry, text, flag;
				if (!entries.GetElement(i, &entry) || !entry.GetMember("text", &text) || !text.IsString() || a_label != text.GetString() ||
					!entry.GetMember("flag", &flag)) {
					continue;
				}
				// changeFilterFlag on the filter also asks the item list to rebuild its view.
				if (a_view->GetVariable(&layout, "Menu.InventoryLists.itemList.layout")) {
					layout.Invoke("changeFilterFlag", nullptr, &flag, 1);
				}
				if (a_view->GetVariable(&filter, "Menu.InventoryLists._typeFilter")) {
					filter.Invoke("changeFilterFlag", nullptr, &flag, 1);
				}
				SKSE::log::info("Catégorie rouverte : {} (filtre {})", a_label, flag.GetNumber());
				return;
			}
			SKSE::log::info("Catégorie {} absente après la mise à jour", a_label);
		}
	}

	void RebuildList()
	{
		const auto subMenu = OpenSubMenu();
		if (!subMenu) {
			return;
		}
		const auto label = OpenCategoryLabel(subMenu->view.get());
		subMenu->UpdateConstructibleList();
		ReopenCategory(subMenu->view.get(), label);
		SKSE::GetTaskInterface()->AddUITask([label] {
			if (const auto menu = OpenSubMenu()) {
				ReopenCategory(menu->view.get(), label);
			}
		});
		RefreshYieldLine();
	}

	void RefreshYieldLine()
	{
		// The game redraws the item card after the list is rebuilt, sometimes a frame later: redo it now and
		// once more from the UI task queue.
		auto refresh = [] {
			const auto ui = RE::UI::GetSingleton();
			const auto menu = ui ? ui->GetMenu<RE::CraftingMenu>() : nullptr;
			if (menu) {
				CraftHooks::ShowYieldLine(skyrim_cast<RE::CraftingSubMenus::ConstructibleObjectMenu*>(menu->subMenu));
			}
		};
		refresh();
		SKSE::GetTaskInterface()->AddUITask(refresh);
	}

	void CreateForms()
	{
		const auto section = RE::BGSKeyword::CreateKeyword("KRI_Recycling");
		const auto factory = RE::IFormFactory::GetConcreteFormFactoryByType<RE::TESObjectMISC>();
		for (std::size_t i = 0; i < g_tokens.size(); ++i) {
			const auto token = factory->Create();
			token->SetFullName("Recyclage");
			token->AddKeyword(section);
			token->AddKeyword(RE::BGSKeyword::CreateKeyword(kCategoryKeywords[i]));
			g_tokens[i] = token;
		}
		SKSE::log::info("Mots-clés et jetons de catégories créés");
	}

	void InstallHooks()
	{
		if (REL::Module::IsVR()) {
			SKSE::log::error("Skyrim VR n'est pas pris en charge : section RECYCLAGE désactivée");
			return;
		}
		// ConstructibleObjectMenu::UpdateItemList -> SetItemEntryData, the call CraftingCategories also wraps.
		// SE 1.5.97: ID 50510 (SE id of AE 51401: CraftingCategories' VR address 0x89B630 minus this region's VR-SE
		// gap 0x2B590 gives SE 0x8700A0), and the call sits at +0x22D as in CraftingCategories' VR build. Untested in
		// game on SE: the 0xE8 check below disables the section rather than patch a wrong address.
		const REL::Relocation<std::uintptr_t> target{ RELOCATION_ID(50510, 51401), REL::Relocate(0x22D, 0x2C8) };
		if (*reinterpret_cast<const std::uint8_t*>(target.address()) != 0xE8) {
			SKSE::log::error("Appel attendu introuvable dans UpdateItemList : section RECYCLAGE désactivée");
			return;
		}
		SetItemEntryData::func = SKSE::GetTrampoline().write_call<5>(target.address(), SetItemEntryData::thunk);

		using SubMenu = RE::CraftingSubMenus::ConstructibleObjectMenu;
		REL::Relocation<std::uintptr_t> subMenuVtable{ RE::VTABLE_CraftingSubMenus__ConstructibleObjectMenu[0] };
		Accept<SubMenu>::func = subMenuVtable.write_vfunc(0x1, Accept<SubMenu>::thunk);
		REL::Relocation<std::uintptr_t> menuVtable{ RE::VTABLE_CraftingMenu[0] };
		Accept<RE::CraftingMenu>::func = menuVtable.write_vfunc(0x1, Accept<RE::CraftingMenu>::thunk);
		REL::Relocation<std::uintptr_t> confirmVtable{ RE::VTABLE_CraftingSubMenus__ConstructibleObjectMenu__CreationConfirmCallback[0] };
		CreationConfirmRun::func = confirmVtable.write_vfunc(0x1, CreationConfirmRun::thunk);
		SKSE::log::info("Hooks du menu de fabrication installés");
	}
}
