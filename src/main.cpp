#include "Orders.h"
#include "RecipeScan.h"
#include "RecycleMenu.h"
#include "Settings.h"
#include "Text.h"

namespace
{
	void OnMessage(SKSE::MessagingInterface::Message* a_msg)
	{
		switch (a_msg->type) {
		case SKSE::MessagingInterface::kPostLoad:
			KRI::RecycleMenu::InstallHooks();
			break;
		case SKSE::MessagingInterface::kDataLoaded:
			KRI::Settings::Load();
			KRI::Text::Load();
			KRI::RecipeScan::Run();
			KRI::RecycleMenu::CreateForms();
			KRI::Orders::Init();
			break;
		default:
			break;
		}
	}
}

SKSEPluginLoad(const SKSE::LoadInterface* a_skse)
{
	// SKSE::Init owns the log (KroosRecycleItems.log); a logger set up before it is replaced.
	SKSE::Init(a_skse, { .logPattern = "[%H:%M:%S] %v", .trampoline = true, .trampolineSize = 64 });
	SKSE::log::info("KroosRecycleItems loaded, game {}", a_skse->RuntimeVersion().string("."));
	KRI::Orders::RegisterSerialization();
	if (!SKSE::GetMessagingInterface()->RegisterListener(OnMessage)) {
		SKSE::log::error("Could not register the SKSE message listener");
		return false;
	}
	return true;
}
