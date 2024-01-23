#include "../api/EventAPI.h"
#include "BuiltinCommands.h"
#include "api/APIHelp.h"
#include "api/EventAPI.h"
#include "api/MoreGlobal.h"
#include "engine/EngineManager.h"
#include "engine/EngineOwnData.h"
#include "engine/GlobalShareData.h"
#include "engine/LocalShareData.h"
#include "engine/MessageSystem.h"
#include "engine/RemoteCall.h"
#include "ll/api/i18n/I18nAPI.h"
#include "main/Configs.h"
#include "main/EconomicSystem.h"
#include "main/SafeGuardRecord.h"
#include "utils/JsonHelper.h"

#include <chrono>
#include <exception>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <windows.h>


#ifdef LLSE_BACKEND_PYTHON
#include "PythonHelper.h"
#endif

// Global vars
ll::Logger logger(LLSE_LOADER_NAME);

extern void LoadDepends();
extern void LoadMain();
extern void BindAPIs(ScriptEngine* engine);
extern void LoadDebugEngine();

void LSE_Load() {
    // Register myself
    // ll::registerPlugin(LLSE_LOADER_NAME, LLSE_LOADER_DESCRIPTION,
    //                    LITELOADER_VERSION,
    //                    {{"GitHub", "github.com/LiteLDev/LiteLoaderBDS"}});

    // Load i18n files
    ll::i18n::load(u8"plugins/LeviLamina/lang");

    // Init global share data
    InitLocalShareData();
    InitGlobalShareData();
    InitSafeGuardRecord();

    // Welcome
    if (localShareData->isFirstInstance) {
        logger.info("ScriptEngine initializing...");
    }

    EconomySystem::init();

#ifdef LLSE_BACKEND_PYTHON
    // This fix is used for Python3.10's bug:
    // The thread will freeze when creating a new engine while another thread is
    // blocking to read stdin Side effects: sys.stdin cannot be used after this
    // patch. More info to see: https://github.com/python/cpython/issues/83526
    //
    // Attention! When CPython is upgraded, this fix must be re-adapted or
    // removed!!
    //
    PythonHelper::FixPython310Stdin::patchPython310CreateStdio();

    PythonHelper::initPythonRuntime();
#endif

    // Pre-load depending libs
    LoadDepends();

    // Load plugins
    LoadMain();

    // Register real-time debug
    LoadDebugEngine();

    // Register basic event listeners
    InitBasicEventListeners();

    // Init message system
    InitMessageSystem();

    MoreGlobal::Init();
}


void LSE_Enable() {
    RegisterDebugCommand();
    LLSECallServerStartedEvent();
}
