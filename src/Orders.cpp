#include "Orders.h"

#include "RecipeScan.h"
#include "Recycler.h"
#include "Settings.h"
#include "Text.h"

#include <random>
#include <thread>

namespace KRI::Orders
{
	namespace
	{
		using Station = RecipeScan::Station;

		// KroosRecycleItems.esl (tools/make-esl.py).
		constexpr auto       kPlugin = "KroosRecycleItems.esl"sv;
		constexpr RE::FormID kOrderFaction = 0x801, kOrderTopic = 0x803, kOrderInfo = 0x804, kPickupTopic = 0x806, kPickupInfo = 0x807,
							 kPaidTopic = 0x808, kChest = 0x80A, kEnabledGlobal = 0x80B;

		// Skyrim.esm.
		constexpr RE::FormID kGold = 0x0000000F;
		constexpr RE::FormID kJobBlacksmith = 0x0005091D;
		constexpr RE::FormID kSharedOfCourse = 0x000DBA22, kSharedHereForYou = 0x000DBA2A;  // lines our topics reuse
		constexpr RE::FormID kThievesGuild = 0x00029DA9, kCompanions = 0x00048362, kImperials = 0x0002BF9A, kStormcloaks = 0x0002BF9B,
							 kPlayerMarried = 0x000C6472;
		constexpr RE::FormID kOrcRace = 0x00013747, kOrcRaceVampire = 0x000A82B9, kNordRace = 0x00013746, kNordRaceVampire = 0x00088794;
		constexpr RE::FormID kOrcStrongholdKeyword = 0x000130E9;
		constexpr RE::FormID kEorlund = 0x00013B9D;
		constexpr RE::FormID kJarlsFavorQuest = 0x00087E24, kRiftenThaneQuest = 0x00065BDF;
		// Dragonborn.esm.
		constexpr RE::FormID kSolstheim = 0x016E2A, kGlover = 0x01828E;

		enum class Hold
		{
			kNone,
			kHaafingar,
			kReach,
			kEastmarch,
			kRift,
			kWhiterun,
			kSmall,  // Falkreath, the Pale, Hjaalmarch, Winterhold
			kSolstheim,
			kStronghold
		};

		struct HoldLocation
		{
			RE::FormID       location;  // Skyrim.esm hold location
			Hold             hold;
			std::string_view thanePrefix;  // FavorJarlsMakeFriendsScript property prefix
		};

		constexpr std::array kHoldLocations{
			HoldLocation{ 0x00016770, Hold::kHaafingar, "Haafingar"sv },
			HoldLocation{ 0x00016769, Hold::kReach, "Reach"sv },
			HoldLocation{ 0x0001676A, Hold::kEastmarch, "Eastmarch"sv },
			HoldLocation{ 0x0001676C, Hold::kRift, "Rift"sv },
			HoldLocation{ 0x00016772, Hold::kWhiterun, "Whiterun"sv },
			HoldLocation{ 0x0001676F, Hold::kSmall, "Falkreath"sv },
			HoldLocation{ 0x0001676D, Hold::kSmall, "Pale"sv },
			HoldLocation{ 0x0001676E, Hold::kSmall, "Hjaalmarch"sv },
			HoldLocation{ 0x0001676B, Hold::kSmall, "Winterhold"sv },
		};

		struct Order
		{
			RE::FormID                                       smith{ 0 };
			float                                            readyDay{ 0.0f };  // GameDaysPassed when it is ready
			std::vector<std::pair<RE::FormID, std::int32_t>> items;
		};

		std::vector<Order> g_orders;

		RE::TESFaction*       g_faction{ nullptr };
		RE::TESTopic*         g_paidTopic{ nullptr };
		RE::TESObjectCONT*    g_chestBase{ nullptr };
		RE::TESGlobal*        g_enabled{ nullptr };
		RE::FormID            g_orderInfo{ 0 };
		RE::FormID            g_pickupInfo{ 0 };
		RE::FormID            g_pendingSmith{ 0 };  // asked for an order: the deposit box opens once the dialogue closes
		RE::ObjectRefHandle   g_chest;              // deposit box being filled
		RE::FormID            g_chestSmith{ 0 };
		std::string           g_pendingNotice;          // "ready in N h": the HUD is hidden during the dialogue
		float                 g_lastReminder{ -1.0f };  // game hours of the last reminder, -1 = not started

		float Today()
		{
			const auto calendar = RE::Calendar::GetSingleton();
			return calendar ? calendar->GetDaysPassed() : 0.0f;
		}

		Order* FindOrder(RE::FormID a_smith)
		{
			const auto it = std::ranges::find(g_orders, a_smith, &Order::smith);
			return it != g_orders.end() ? std::to_address(it) : nullptr;
		}

		RE::PlayerCharacter* Player()
		{
			return RE::PlayerCharacter::GetSingleton();
		}

		bool PlayerIn(RE::FormID a_faction)
		{
			const auto faction = RE::TESForm::LookupByID<RE::TESFaction>(a_faction);
			return faction && Player()->IsInFaction(faction);
		}

		bool PlayerIsRace(std::initializer_list<RE::FormID> a_races)
		{
			const auto race = Player()->GetRace();
			return race && std::ranges::find(a_races, race->GetFormID()) != a_races.end();
		}

		bool IsGlover(RE::Actor* a_smith)
		{
			const auto glover = RE::TESDataHandler::GetSingleton()->LookupFormID(kGlover, "Dragonborn.esm"sv);
			const auto base = a_smith->GetActorBase();
			return glover && base && base->GetFormID() == glover;
		}

		// The hold the blacksmith works in, and the location whose name the price box shows.
		std::pair<Hold, const RE::BGSLocation*> HoldOf(RE::Actor* a_smith, std::string_view* a_thanePrefix = nullptr)
		{
			const auto stronghold = RE::TESForm::LookupByID<RE::BGSKeyword>(kOrcStrongholdKeyword);
			const auto solstheim = RE::TESDataHandler::GetSingleton()->LookupFormID(kSolstheim, "Dragonborn.esm"sv);
			for (auto location = a_smith->GetCurrentLocation(); location; location = location->parentLoc) {
				if (stronghold && location->HasKeyword(stronghold)) {
					return { Hold::kStronghold, location };
				}
				if (location->GetFormID() == solstheim) {
					return { Hold::kSolstheim, location };
				}
				const auto it = std::ranges::find(kHoldLocations, location->GetFormID(), &HoldLocation::location);
				if (it != kHoldLocations.end()) {
					if (a_thanePrefix) {
						*a_thanePrefix = it->thanePrefix;
					}
					return { it->hold, location };
				}
			}
			return { Hold::kNone, nullptr };
		}

		// A bool or int property of a quest's script, read through the Papyrus VM.
		bool QuestFlag(RE::FormID a_quest, const char* a_script, const std::string& a_property)
		{
			const auto quest = RE::TESForm::LookupByID<RE::TESQuest>(a_quest);
			const auto vm = RE::BSScript::Internal::VirtualMachine::GetSingleton();
			const auto policy = vm ? vm->GetObjectHandlePolicy() : nullptr;
			if (!quest || !policy) {
				return false;
			}
			RE::BSTSmartPointer<RE::BSScript::Object> object;
			if (!vm->FindBoundObject(policy->GetHandleForObject(RE::TESQuest::FORMTYPE, quest), a_script, object) || !object) {
				return false;
			}
			const auto variable = object->GetProperty(a_property);
			return variable && ((variable->IsBool() && variable->GetBool()) || (variable->IsInt() && variable->GetSInt() != 0));
		}

		// Thane of the hold, under its Imperial or its Stormcloak jarl (the guards' "my Thane" uses the same flags).
		bool IsThane(std::string_view a_prefix)
		{
			const auto prefix = std::string{ a_prefix };
			if (QuestFlag(kJarlsFavorQuest, "FavorJarlsMakeFriendsScript", prefix + "ImpGetOutofJail") ||
				QuestFlag(kJarlsFavorQuest, "FavorJarlsMakeFriendsScript", prefix + "SonsGetOutofJail")) {
				return true;
			}
			// Riften's thanehood is its own quest.
			const auto riften = RE::TESForm::LookupByID<RE::TESQuest>(kRiftenThaneQuest);
			return a_prefix == "Rift"sv && riften && riften->IsCompleted();
		}

		std::string Signed(float a_fraction)
		{
			const auto percent = static_cast<int>(std::lround(a_fraction * 100.0f));
			return Text::Format("$KRI_Percent", fmt::format("{}{}", percent > 0 ? "+" : "", percent));
		}

		struct Price
		{
			std::int32_t             total{ 0 };
			std::vector<std::string> notes;  // "Haafingar : +50 %", "Thane : -20 %"...
		};

		Price PriceOf(RE::Actor* a_smith, const std::vector<std::pair<RE::TESBoundObject*, std::int32_t>>& a_items)
		{
			const auto& settings = Settings::Get();
			Price       price;

			float base = 0.0f;
			for (const auto& [item, count] : a_items) {
				const auto lot = RecipeScan::LotSize(item);
				const auto unit = std::max(settings.orderMinPrice, static_cast<float>(std::max(item->GetGoldValue(), 0) * lot) * settings.orderShare);
				base += unit * static_cast<float>(count / lot);
			}

			std::string_view thanePrefix;
			const auto [hold, location] = HoldOf(a_smith, &thanePrefix);
			const auto holdName = location && location->GetName() && *location->GetName() ? std::string{ location->GetName() } : std::string{};
			float factor = 1.0f;
			auto  apply = [&](float a_change, const std::string& a_label) {
				if (a_change != 0.0f) {
					factor *= 1.0f + a_change;
					price.notes.push_back(fmt::format("{} : {}", a_label, Signed(a_change)));
				}
			};
			const float holdFactor = hold == Hold::kHaafingar  ? settings.holdHaafingar :
			                         hold == Hold::kReach      ? settings.holdReach :
			                         hold == Hold::kEastmarch  ? settings.holdEastmarch :
			                         hold == Hold::kRift       ? settings.holdRift :
			                         hold == Hold::kWhiterun   ? settings.holdWhiterun :
			                         hold == Hold::kSmall      ? settings.holdSmall :
			                         hold == Hold::kSolstheim  ? settings.holdSolstheim :
			                         hold == Hold::kStronghold ? settings.holdStronghold :
			                                                     1.0f;
			apply(holdFactor - 1.0f, holdName.empty() ? Text::Get("$KRI_PriceHold") : holdName);

			if (hold == Hold::kStronghold && PlayerIsRace({ kOrcRace, kOrcRaceVampire })) {
				apply(-settings.discountOrc, Text::Get("$KRI_PriceOrc"));
			}
			if (hold == Hold::kEastmarch && !PlayerIsRace({ kNordRace, kNordRaceVampire })) {
				apply(settings.surchargeNonNord, Text::Get("$KRI_PriceNonNord"));
			}
			if (hold == Hold::kRift && PlayerIn(kThievesGuild)) {
				apply(-settings.discountThieves, Text::Get("$KRI_PriceThieves"));
			}
			if (!thanePrefix.empty() && IsThane(thanePrefix)) {
				apply(-settings.discountThane, Text::Get("$KRI_PriceThane"));
			}
			const auto base_ = a_smith->GetActorBase();
			if (base_ && base_->GetFormID() == kEorlund && PlayerIn(kCompanions)) {
				apply(-settings.discountCompanions, Text::Get("$KRI_PriceCompanions"));
			}
			if (hold == Hold::kHaafingar && PlayerIn(kImperials)) {
				apply(-settings.discountCivilWar, Text::Get("$KRI_PriceLegion"));
			}
			if (hold == Hold::kEastmarch && PlayerIn(kStormcloaks)) {
				apply(-settings.discountCivilWar, Text::Get("$KRI_PriceStormcloaks"));
			}

			price.total = std::max(static_cast<std::int32_t>(std::lround(base * factor)), 1);
			const auto married = RE::TESForm::LookupByID<RE::TESFaction>(kPlayerMarried);
			if (settings.spouseFree && married && a_smith->IsInFaction(married)) {
				price.total = 0;
				price.notes.push_back(Text::Get("$KRI_PriceSpouse"));
			}
			return price;
		}

		// Make the blacksmith say the paid line (Papyrus ObjectReference.Say, dispatched from here).
		void SayPaid(RE::Actor* a_smith)
		{
			const auto vm = RE::BSScript::Internal::VirtualMachine::GetSingleton();
			const auto policy = vm ? vm->GetObjectHandlePolicy() : nullptr;
			if (!policy || !g_paidTopic) {
				return;
			}
			const auto handle = policy->GetHandleForObject(RE::Actor::FORMTYPE, a_smith);
			const auto args = RE::MakeFunctionArguments(static_cast<RE::TESTopic*>(g_paidTopic), static_cast<RE::Actor*>(nullptr), false);
			RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> callback;
			if (!vm->DispatchMethodCall(handle, "ObjectReference", "Say", args, callback)) {
				SKSE::log::warn("Commande : réplique de paiement non jouée ({} n'a pas d'objet Papyrus)", a_smith->GetName());
			}
		}

		void SetState(RE::Actor* a_smith, std::int8_t a_rank)
		{
			if (a_rank < 0) {
				a_smith->RemoveFromFaction(g_faction);
			} else {
				a_smith->AddToFaction(g_faction, a_rank);
			}
		}

		void DeleteChest()
		{
			if (const auto chest = g_chest.get()) {
				chest->Disable();
				chest->SetDelete(true);
			}
			g_chest = {};
			g_chestSmith = 0;
		}

		// Give a_count copies of a_item back to the player, keeping enchanted / improved copies as they are.
		void GiveBack(RE::TESObjectREFR* a_chest, RE::TESBoundObject* a_item, std::int32_t a_count)
		{
			const auto player = Player();
			auto       inventory = a_chest->GetInventory([a_item](RE::TESBoundObject& a_obj) { return &a_obj == a_item; });
			const auto it = inventory.find(a_item);
			if (it != inventory.end() && it->second.second && it->second.second->extraLists) {
				std::vector<RE::ExtraDataList*> lists{ it->second.second->extraLists->begin(), it->second.second->extraLists->end() };
				for (const auto xList : lists) {
					if (a_count <= 0) {
						return;
					}
					const auto count = std::min(xList ? xList->GetCount() : 0, a_count);
					if (count > 0) {
						a_chest->RemoveItem(a_item, count, RE::ITEM_REMOVE_REASON::kStoreInContainer, xList, player);
						a_count -= count;
					}
				}
			}
			if (a_count > 0) {
				a_chest->RemoveItem(a_item, a_count, RE::ITEM_REMOVE_REASON::kStoreInContainer, nullptr, player);
			}
		}

		void OpenDeposit(RE::FormID a_smith)
		{
			const auto smith = RE::TESForm::LookupByID<RE::Actor>(a_smith);
			const auto player = Player();
			if (!smith || !player || !g_chestBase) {
				return;
			}
			DeleteChest();
			const auto chest = player->PlaceObjectAtMe(g_chestBase, false);
			if (!chest) {
				SKSE::log::error("Commande : impossible de créer le coffre de dépôt");
				return;
			}
			// Out of sight under the player; the container menu does not care where the box is.
			auto position = player->GetPosition();
			position.z -= 3000.0f;
			chest->SetPosition(position);
			g_chest = chest->GetHandle();
			g_chestSmith = a_smith;
			chest->ActivateRef(player, 0, nullptr, 1, false);
		}

		std::string ItemList(const std::vector<std::pair<RE::TESBoundObject*, std::int32_t>>& a_items)
		{
			std::string list;
			for (const auto& [item, count] : a_items) {
				list += fmt::format("\n- {} x{}", item->GetName(), count);
			}
			return list;
		}

		std::string ExpectedYields(const std::vector<std::pair<RE::TESBoundObject*, std::int32_t>>& a_items)
		{
			std::string text;
			for (const auto& [item, count] : a_items) {
				auto station = RecipeScan::StationOf(item);
				station = station == Station::kBoth ? Station::kSmelter : station;
				std::string labels;
				for (const auto& yield : Recycler::ComputeYields(item, station, Settings::Get().orderSkill)) {
					const auto name = yield.material->GetName();
					const auto percent = Text::Format("$KRI_Percent", static_cast<int>(std::lround(yield.chance * 100.0f)));
					labels += (labels.empty() ? "" : ", ") +
					          (yield.guaranteed > 0 && yield.chance > 0.0f ? Text::Format("$KRI_YieldSurePlus", name, yield.guaranteed, percent) :
					              yield.guaranteed > 0                     ? Text::Format("$KRI_YieldSure", name, yield.guaranteed) :
					                                                         Text::Format("$KRI_YieldChance", name, percent));
				}
				const auto lot = RecipeScan::LotSize(item);
				const auto name = lot > 1 ? Text::Format("$KRI_YieldSure", item->GetName(), lot) : std::string{ item->GetName() };
				text += fmt::format("\n- {} : {}", name, labels.empty() ? Text::Get("$KRI_Nothing") : labels);
			}
			return text;
		}

		class PayCallback : public RE::IMessageBoxCallback
		{
		public:
			PayCallback(std::vector<std::pair<RE::TESBoundObject*, std::int32_t>> a_items, std::int32_t a_price, bool a_canPay) :
				_items(std::move(a_items)),
				_price(a_price),
				_canPay(a_canPay)
			{}

			void Run(std::uint8_t a_button) override
			{
				const auto chest = g_chest.get();
				const auto smith = RE::TESForm::LookupByID<RE::Actor>(g_chestSmith);
				if (!chest || !smith) {
					return;
				}
				if (a_button != 0 || !_canPay) {
					for (const auto& [item, count] : _items) {
						GiveBack(chest.get(), item, count);
					}
					DeleteChest();
					return;
				}
				const auto player = Player();
				if (_price > 0) {
					player->RemoveItem(RE::TESForm::LookupByID<RE::TESBoundObject>(kGold), _price, RE::ITEM_REMOVE_REASON::kRemove, nullptr, nullptr);
				}
				Order order{ smith->GetFormID(), Today() + Settings::Get().orderHours / 24.0f, {} };
				for (const auto& [item, count] : _items) {
					order.items.emplace_back(item->GetFormID(), count);
				}
				g_orders.push_back(std::move(order));
				DeleteChest();  // the items go with it: the blacksmith has them now
				SetState(smith, 0);
				// The price box pauses the game: say the line 3 s later, once it has resumed (Kevin, 2026-10-30).
				std::thread([id = smith->GetFormID()] {
					std::this_thread::sleep_for(3s);
					SKSE::GetTaskInterface()->AddTask([id] {
						if (const auto actor = RE::TESForm::LookupByID<RE::Actor>(id)) {
							SayPaid(actor);
						}
					});
				}).detach();
				RE::SendHUDMessage::ShowHUDMessage(Text::Format("$KRI_OrderPlaced", static_cast<int>(Settings::Get().orderHours)).c_str());
				SKSE::log::info("Commande passée chez {} : {} objets, {} po", smith->GetName(), _items.size(), _price);
			}

		private:
			std::vector<std::pair<RE::TESBoundObject*, std::int32_t>> _items;
			std::int32_t                                              _price;
			bool                                                      _canPay;
		};

		// The deposit box was closed: keep what this blacksmith recycles, give back the rest, show the price.
		void ProcessDeposit()
		{
			const auto chest = g_chest.get();
			const auto smith = RE::TESForm::LookupByID<RE::Actor>(g_chestSmith);
			if (!chest || !smith) {
				DeleteChest();
				return;
			}
			std::vector<std::pair<RE::TESBoundObject*, std::int32_t>> accepted;
			std::vector<std::pair<RE::TESBoundObject*, std::int32_t>> refused;
			for (const auto& [item, data] : chest->GetInventory()) {
				const auto count = data.first;
				const auto station = RecipeScan::StationOf(item);
				// Staves only at Glover Mallory, Raven Rock (Kevin, 2026-10-10).
				const bool takes = station != Station::kNone && (station != Station::kStaffEnchanter || IsGlover(smith));
				const auto lot = RecipeScan::LotSize(item);
				const auto take = takes ? count / lot * lot : 0;
				if (take > 0) {
					accepted.emplace_back(item, take);
				}
				if (count > take) {
					refused.emplace_back(item, count - take);
				}
			}
			for (const auto& [item, count] : refused) {
				GiveBack(chest.get(), item, count);
			}
			if (accepted.empty()) {
				DeleteChest();
				if (!refused.empty()) {
					RE::SendHUDMessage::ShowHUDMessage(Text::Get("$KRI_OrderNothing").c_str());
				}
				return;
			}

			const auto price = PriceOf(smith, accepted);
			const auto gold = Player()->GetGoldAmount();
			std::string body = Text::Format("$KRI_OrderTitle", smith->GetName()) + ItemList(accepted);
			if (!refused.empty()) {
				body += "\n\n" + Text::Get("$KRI_OrderRefused") + ItemList(refused);
			}
			body += "\n\n" + Text::Get("$KRI_OrderYieldHeader") + ExpectedYields(accepted);
			body += "\n\n" + Text::Format("$KRI_OrderPrice", price.total);
			for (const auto& note : price.notes) {
				body += "\n" + note;
			}
			body += "\n" + Text::Format("$KRI_OrderReady", static_cast<int>(Settings::Get().orderHours));
			const bool canPay = gold >= price.total;
			if (!canPay) {
				body += "\n\n" + Text::Format("$KRI_OrderNoGold", price.total - gold);
			}

			RE::BSString               text{ body.c_str() };
			RE::BSTArray<RE::BSString> buttons;
			if (canPay) {
				buttons.push_back(RE::BSString{ Text::Get("$KRI_OrderPay").c_str() });
				buttons.push_back(RE::BSString{ Text::Get("$KRI_OrderCancel").c_str() });
			} else {
				buttons.push_back(RE::BSString{ Text::GameSetting("sOk", "OK").c_str() });
			}
			const RE::BSTSmartPointer<RE::IMessageBoxCallback> callback{ new PayCallback(accepted, price.total, canPay) };
			RE::MessageBoxMenu::Create(text, callback, 0, 4, 10, buttons);
		}

		// Ready orders show the pickup topic; others tell the player how long to wait.
		void Refresh(RE::Actor* a_smith, bool a_notify)
		{
			const auto order = FindOrder(a_smith->GetFormID());
			if (!order) {
				return;
			}
			const auto left = order->readyDay - Today();
			SetState(a_smith, left <= 0.0f ? 1 : 0);
			if (left > 0.0f && a_notify) {
				const auto hours = std::max(static_cast<int>(std::ceil(left * 24.0f)), 1);
				g_pendingNotice = Text::Format("$KRI_OrderNotReady", a_smith->GetName(), hours);
			}
		}

		// MCM reminders, checked every few seconds: one notification per order every orderReminderHours game hours.
		void CheckReminders()
		{
			const auto& settings = Settings::Get();
			const float now = Today() * 24.0f;
			if (!settings.orderReminders || g_orders.empty() || g_lastReminder < 0.0f) {
				g_lastReminder = now;
				return;
			}
			if (now - g_lastReminder < settings.orderReminderHours) {
				return;
			}
			g_lastReminder = now;
			for (const auto& order : g_orders) {
				const auto smith = RE::TESForm::LookupByID<RE::Actor>(order.smith);
				if (!smith) {
					continue;
				}
				const auto left = order.readyDay - Today();
				const auto text = left <= 0.0f ? Text::Format("$KRI_OrderReminderReady", smith->GetName()) :
				                                 Text::Format("$KRI_OrderNotReady", smith->GetName(), std::max(static_cast<int>(std::ceil(left * 24.0f)), 1));
				RE::SendHUDMessage::ShowHUDMessage(text.c_str());
			}
		}

		void Pickup(RE::Actor* a_smith)
		{
			const auto it = std::ranges::find(g_orders, a_smith->GetFormID(), &Order::smith);
			if (it == g_orders.end() || it->readyDay > Today()) {
				return;
			}
			static std::mt19937                                       rng{ std::random_device{}() };
			std::uniform_real_distribution<float>                     roll{ 0.0f, 1.0f };
			std::vector<std::pair<RE::TESBoundObject*, std::int32_t>> totals;
			for (const auto& [formID, count] : it->items) {
				const auto item = RE::TESForm::LookupByID<RE::TESBoundObject>(formID);
				auto       station = item ? RecipeScan::StationOf(item) : Station::kNone;
				if (station == Station::kNone) {
					continue;
				}
				station = station == Station::kBoth ? Station::kSmelter : station;
				const auto yields = Recycler::ComputeYields(item, station, Settings::Get().orderSkill);
				for (auto times = count / RecipeScan::LotSize(item); times > 0; --times) {
					for (const auto& yield : yields) {
						const auto amount = yield.guaranteed + (yield.chance > 0.0f && roll(rng) < yield.chance ? 1 : 0);
						if (amount <= 0) {
							continue;
						}
						const auto total = std::ranges::find(totals, yield.material, &std::pair<RE::TESBoundObject*, std::int32_t>::first);
						if (total != totals.end()) {
							total->second += amount;
						} else {
							totals.emplace_back(yield.material, amount);
						}
					}
				}
			}
			g_orders.erase(it);
			SetState(a_smith, -1);

			std::string summary;
			for (const auto& [material, amount] : totals) {
				Player()->AddObjectToContainer(material, nullptr, amount, nullptr);
				summary += fmt::format("{}{} x{}", summary.empty() ? "" : ", ", material->GetName(), amount);
			}
			SKSE::log::info("Commande récupérée chez {} : {}", a_smith->GetName(), summary.empty() ? "rien"s : summary);
			RE::BSString               text{ (summary.empty() ? Text::Get("$KRI_ResultNothing") : Text::Format("$KRI_Result", summary)).c_str() };
			RE::BSTArray<RE::BSString> buttons;
			buttons.push_back(RE::BSString{ Text::GameSetting("sOk", "OK").c_str() });
			RE::MessageBoxMenu::Create(text, RE::BSTSmartPointer<RE::IMessageBoxCallback>{}, 0, 4, 10, buttons);
		}

		void AddTask(std::function<void()> a_task)
		{
			SKSE::GetTaskInterface()->AddTask(std::move(a_task));
		}

		class Events :
			public RE::BSTEventSink<RE::TESTopicInfoEvent>,
			public RE::BSTEventSink<RE::TESActivateEvent>,
			public RE::BSTEventSink<RE::MenuOpenCloseEvent>
		{
		public:
			RE::BSEventNotifyControl ProcessEvent(const RE::TESTopicInfoEvent* a_event, RE::BSTEventSource<RE::TESTopicInfoEvent>*) override
			{
				if (!a_event || !a_event->speakerRef || a_event->type.get() != RE::TESTopicInfoEvent::TopicInfoEventType::kTopicEnd) {
					return RE::BSEventNotifyControl::kContinue;
				}
				const auto smith = a_event->speakerRef->GetFormID();
				// The event may name our line or the vanilla shared line it reuses (never spoken on its own).
				const auto info = a_event->topicInfoFormID;
				if (info == g_orderInfo || info == g_pickupInfo || info == kSharedOfCourse || info == kSharedHereForYou) {
					SKSE::log::info("Commande : réplique {:08X} de {} terminée", info, a_event->speakerRef->GetName());
				}
				if (info == g_orderInfo || info == kSharedOfCourse) {
					g_pendingSmith = smith;
					// The line usually ends after the dialogue closed: open the box now, otherwise when it closes.
					if (!RE::UI::GetSingleton()->IsMenuOpen(RE::DialogueMenu::MENU_NAME)) {
						g_pendingSmith = 0;
						AddTask([smith] { OpenDeposit(smith); });
					}
				} else if (info == g_pickupInfo || info == kSharedHereForYou) {
					AddTask([smith] {
						if (const auto actor = RE::TESForm::LookupByID<RE::Actor>(smith)) {
							Pickup(actor);
						}
					});
				}
				return RE::BSEventNotifyControl::kContinue;
			}

			RE::BSEventNotifyControl ProcessEvent(const RE::TESActivateEvent* a_event, RE::BSTEventSource<RE::TESActivateEvent>*) override
			{
				if (a_event && a_event->actionRef && a_event->actionRef->IsPlayerRef() && a_event->objectActivated) {
					const auto actor = a_event->objectActivated->As<RE::Actor>();
					const auto blacksmiths = RE::TESForm::LookupByID<RE::TESFaction>(kJobBlacksmith);
					if (actor && blacksmiths && actor->IsInFaction(blacksmiths)) {
						// Before the dialogue opens: the MCM may have turned orders on or off, the order may be ready.
						Settings::Load();
						ApplySettings();
						if (FindOrder(actor->GetFormID())) {
							Refresh(actor, true);
						}
					}
				}
				return RE::BSEventNotifyControl::kContinue;
			}

			RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent* a_event, RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override
			{
				if (!a_event || a_event->opening) {
					return RE::BSEventNotifyControl::kContinue;
				}
				if (a_event->menuName == RE::DialogueMenu::MENU_NAME && !g_pendingNotice.empty()) {
					AddTask([notice = std::exchange(g_pendingNotice, {})] { RE::SendHUDMessage::ShowHUDMessage(notice.c_str()); });
				}
				if (a_event->menuName == RE::DialogueMenu::MENU_NAME && g_pendingSmith) {
					const auto smith = std::exchange(g_pendingSmith, 0);
					AddTask([smith] { OpenDeposit(smith); });
				} else if (a_event->menuName == RE::ContainerMenu::MENU_NAME && g_chest) {
					AddTask(ProcessDeposit);
				}
				return RE::BSEventNotifyControl::kContinue;
			}
		};

		Events g_events;

		constexpr std::uint32_t kRecord = static_cast<std::uint32_t>('ORDR');
		constexpr std::uint32_t kVersion = 1;

		void Save(SKSE::SerializationInterface* a_intfc)
		{
			if (!a_intfc->OpenRecord(kRecord, kVersion)) {
				return;
			}
			a_intfc->WriteRecordData(static_cast<std::uint32_t>(g_orders.size()));
			for (const auto& order : g_orders) {
				a_intfc->WriteRecordData(order.smith);
				a_intfc->WriteRecordData(order.readyDay);
				a_intfc->WriteRecordData(static_cast<std::uint32_t>(order.items.size()));
				for (const auto& [formID, count] : order.items) {
					a_intfc->WriteRecordData(formID);
					a_intfc->WriteRecordData(count);
				}
			}
		}

		void Load(SKSE::SerializationInterface* a_intfc)
		{
			g_orders.clear();
			std::uint32_t type, version, length;
			while (a_intfc->GetNextRecordInfo(type, version, length)) {
				if (type != kRecord || version != kVersion) {
					continue;
				}
				std::uint32_t orders = 0;
				a_intfc->ReadRecordData(orders);
				for (std::uint32_t i = 0; i < orders; ++i) {
					Order         order;
					std::uint32_t items = 0;
					a_intfc->ReadRecordData(order.smith);
					a_intfc->ReadRecordData(order.readyDay);
					a_intfc->ReadRecordData(items);
					for (std::uint32_t j = 0; j < items; ++j) {
						RE::FormID   formID = 0;
						std::int32_t count = 0;
						a_intfc->ReadRecordData(formID);
						a_intfc->ReadRecordData(count);
						if (a_intfc->ResolveFormID(formID, formID)) {
							order.items.emplace_back(formID, count);
						}
					}
					if (a_intfc->ResolveFormID(order.smith, order.smith)) {
						g_orders.push_back(std::move(order));
					}
				}
			}
			SKSE::log::info("Commandes en cours chargées : {}", g_orders.size());
		}

		void Revert(SKSE::SerializationInterface*)
		{
			g_orders.clear();
			g_pendingSmith = 0;
			g_chest = {};
			g_chestSmith = 0;
			g_pendingNotice.clear();
			g_lastReminder = -1.0f;
		}
	}

	void Init()
	{
		const auto dataHandler = RE::TESDataHandler::GetSingleton();
		g_faction = dataHandler->LookupForm<RE::TESFaction>(kOrderFaction, kPlugin);
		g_paidTopic = dataHandler->LookupForm<RE::TESTopic>(kPaidTopic, kPlugin);
		g_chestBase = dataHandler->LookupForm<RE::TESObjectCONT>(kChest, kPlugin);
		g_enabled = dataHandler->LookupForm<RE::TESGlobal>(kEnabledGlobal, kPlugin);
		g_orderInfo = dataHandler->LookupFormID(kOrderInfo, kPlugin);
		g_pickupInfo = dataHandler->LookupFormID(kPickupInfo, kPlugin);
		if (!g_faction || !g_paidTopic || !g_chestBase || !g_enabled || !g_orderInfo || !g_pickupInfo) {
			SKSE::log::error("Commandes chez un forgeron désactivées : formes introuvables dans {}", kPlugin);
			return;
		}
		// The plugin carries English text; show the topics and the box in the game's language.
		if (const auto topic = dataHandler->LookupForm<RE::TESTopic>(kOrderTopic, kPlugin)) {
			topic->fullName = Text::Get("$KRI_OrderTopic");
		}
		if (const auto topic = dataHandler->LookupForm<RE::TESTopic>(kPickupTopic, kPlugin)) {
			topic->fullName = Text::Get("$KRI_PickupTopic");
		}
		g_chestBase->fullName = Text::Get("$KRI_OrderChest");
		ApplySettings();

		const auto events = RE::ScriptEventSourceHolder::GetSingleton();
		events->AddEventSink<RE::TESTopicInfoEvent>(&g_events);
		events->AddEventSink<RE::TESActivateEvent>(&g_events);
		RE::UI::GetSingleton()->AddEventSink<RE::MenuOpenCloseEvent>(&g_events);
		std::thread([] {
			for (;;) {
				std::this_thread::sleep_for(5s);
				SKSE::GetTaskInterface()->AddTask(CheckReminders);
			}
		}).detach();
		SKSE::log::info("Commandes chez un forgeron prêtes");
	}

	void ApplySettings()
	{
		if (g_enabled) {
			g_enabled->value = Settings::Get().ordersEnabled ? 1.0f : 0.0f;
		}
	}

	void RegisterSerialization()
	{
		const auto serialization = SKSE::GetSerializationInterface();
		serialization->SetUniqueID(static_cast<std::uint32_t>('KRIO'));
		serialization->SetSaveCallback(Save);
		serialization->SetLoadCallback(Load);
		serialization->SetRevertCallback(Revert);
	}
}
