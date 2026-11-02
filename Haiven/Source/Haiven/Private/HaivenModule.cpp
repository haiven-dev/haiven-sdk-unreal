#include "Modules/ModuleManager.h"
#include "WebSocketsModule.h"

class FHaivenModule final : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		FModuleManager::LoadModuleChecked<FWebSocketsModule>(TEXT("WebSockets"));
	}
};

IMPLEMENT_MODULE(FHaivenModule, Haiven)
