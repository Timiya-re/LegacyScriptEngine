#include "PluginManager.h"

#include "Entry.h"
#include "Plugin.h"
#include "legacy/api/EventAPI.h"
#include "legacy/engine/EngineManager.h"
#include "legacy/engine/EngineOwnData.h"

#include <ScriptX/ScriptX.h>
#include <exception>
#include <filesystem>
#include <fmt/format.h>
#include <ll/api/Logger.h>
#include <ll/api/io/FileUtils.h>
#include <ll/api/mod/Mod.h>
#include <ll/api/mod/ModManager.h>
#include <ll/api/service/ServerInfo.h>
#include <ll/api/utils/StringUtils.h>
#include <memory>

#ifdef LEGACY_SCRIPT_ENGINE_BACKEND_LUA

constexpr auto BaseLibFileName   = "BaseLib.lua";
constexpr auto PluginManagerName = "lse-lua";

#endif

#ifdef LEGACY_SCRIPT_ENGINE_BACKEND_QUICKJS

constexpr auto BaseLibFileName   = "BaseLib.js";
constexpr auto PluginManagerName = "lse-quickjs";

#endif

#ifdef LEGACY_SCRIPT_ENGINE_BACKEND_PYTHON

#include "legacy/main/PythonHelper.h"
constexpr auto BaseLibFileName   = "BaseLib.py";
constexpr auto PluginManagerName = "lse-python";

#endif

#ifdef LEGACY_SCRIPT_ENGINE_BACKEND_NODEJS

#include "legacy/main/NodeJsHelper.h"
constexpr auto PluginManagerName = "lse-nodejs";

#endif

// Do not use legacy headers directly, otherwise there will be tons of errors.
void BindAPIs(script::ScriptEngine* engine);
void LLSERemoveTimeTaskData(script::ScriptEngine* engine);
auto LLSERemoveAllEventListeners(script::ScriptEngine* engine) -> bool;
auto LLSERemoveCmdRegister(script::ScriptEngine* engine) -> bool;
auto LLSERemoveCmdCallback(script::ScriptEngine* engine) -> bool;
auto LLSERemoveAllExportedFuncs(script::ScriptEngine* engine) -> bool;

namespace lse {

PluginManager::PluginManager() : ll::mod::ModManager(PluginManagerName) {}

ll::Expected<> PluginManager::load(ll::mod::Manifest manifest) {
    auto& logger = getSelfPluginInstance().getLogger();
#ifdef LEGACY_SCRIPT_ENGINE_BACKEND_PYTHON
    std::filesystem::path dirPath = ll::mod::getModsRoot() / manifest.name; // Plugin path
    std::string           entryPath =
        PythonHelper::findEntryScript(ll::string_utils::u8str2str(dirPath.u8string())); // Plugin entry
    // if (entryPath.empty()) return false;
    // std::string pluginName = PythonHelper::getPluginPackageName(dirPath.string()); // Plugin name

    // Run "pip install" if needed
    auto realPackageInstallDir = (std::filesystem::path(dirPath) / "site-packages").make_preferred();
    if (!std::filesystem::exists(realPackageInstallDir)) {
        std::string dependTmpFilePath =
            PythonHelper::getPluginPackDependencyFilePath(ll::string_utils::u8str2str(dirPath.u8string()));
        if (!dependTmpFilePath.empty()) {
            int exitCode = 0;
            lse::getSelfPluginInstance().getLogger().info("llse.loader.python.executePipInstall.start"_tr(
                fmt::arg("name", ll::string_utils::u8str2str(dirPath.filename().u8string()))
            ));

            if ((exitCode = PythonHelper::executePipCommand(
                     "pip install -r \"" + dependTmpFilePath + "\" -t \""
                     + ll::string_utils::u8str2str(realPackageInstallDir.u8string()) + "\" --disable-pip-version-check "
                 ))
                == 0) {
                lse::getSelfPluginInstance().getLogger().info("llse.loader.python.executePipInstall.success"_tr());
            } else
                lse::getSelfPluginInstance().getLogger().error(
                    "llse.loader.python.executePipInstall.fail"_tr(fmt::arg("code", exitCode))
                );

            // remove temp dependency file after installation
            std::error_code ec;
            std::filesystem::remove(std::filesystem::path(dependTmpFilePath), ec);
        }
    }
#endif
#ifdef LEGACY_SCRIPT_ENGINE_BACKEND_NODEJS
    std::filesystem::path dirPath = ll::mod::getModsRoot() / manifest.name; // Plugin path
    // std::string           entryPath = NodeJsHelper::findEntryScript(dirPath.string()); // Plugin entry
    // if (entryPath.empty()) return false;
    // std::string pluginName = NodeJsHelper::getPluginPackageName(dirPath.string()); // Plugin name

    // Run "npm install" if needed
    if (NodeJsHelper::doesPluginPackHasDependency(ll::string_utils::u8str2str(dirPath.u8string()))
        && !std::filesystem::exists(std::filesystem::path(dirPath) / "node_modules")) {
        int exitCode = 0;
        lse::getSelfPluginInstance().getLogger().info("llse.loader.nodejs.executeNpmInstall.start"_tr(
            fmt::arg("name", ll::string_utils::u8str2str(dirPath.filename().u8string()))
        ));
        if ((exitCode = NodeJsHelper::executeNpmCommand("npm install", ll::string_utils::u8str2str(dirPath.u8string())))
            == 0)
            lse::getSelfPluginInstance().getLogger().info("llse.loader.nodejs.executeNpmInstall.success"_tr());
        else
            lse::getSelfPluginInstance().getLogger().error(
                "llse.loader.nodejs.executeNpmInstall.fail"_tr(fmt::arg("code", exitCode))
            );
    }
#endif

    logger.info("loading plugin {}", manifest.name);

    if (hasMod(manifest.name)) {
        return ll::makeStringError("plugin already loaded");
    }

    auto& scriptEngine = *EngineManager::newEngine(manifest.name);
    auto  plugin       = std::make_shared<Plugin>(manifest);

    try {
        script::EngineScope engineScope(scriptEngine);

        // Set plugins's logger title
        ENGINE_OWN_DATA()->logger.title = manifest.name;
        ENGINE_OWN_DATA()->pluginName   = manifest.name;

#ifdef LEGACY_SCRIPT_ENGINE_BACKEND_PYTHON
        scriptEngine.eval("import sys as _llse_py_sys_module");
        std::error_code ec;

        // add plugin-own site-packages to sys.path
        string pluginSitePackageFormatted = ll::string_utils::u8str2str(
            std::filesystem::canonical(realPackageInstallDir.make_preferred(), ec).u8string()
        );
        if (!ec) {
            scriptEngine.eval("_llse_py_sys_module.path.insert(0, r'" + pluginSitePackageFormatted + "')");
        }
        // add plugin source dir to sys.path
        string sourceDirFormatted =
            ll::string_utils::u8str2str(std::filesystem::canonical(dirPath.make_preferred()).u8string());
        scriptEngine.eval("_llse_py_sys_module.path.insert(0, r'" + sourceDirFormatted + "')");

        // set __file__ and __name__
        string entryPathFormatted = ll::string_utils::u8str2str(
            std::filesystem::canonical(std::filesystem::path(entryPath).make_preferred()).u8string()
        );
        scriptEngine.set("__file__", entryPathFormatted);
        // engine->set("__name__", String::newString("__main__"));
#endif

        BindAPIs(&scriptEngine);

        auto& self = getSelfPluginInstance();
#ifndef LEGACY_SCRIPT_ENGINE_BACKEND_NODEJS // NodeJs backend load depends code in another place
        // Load BaseLib.
        auto baseLibPath    = self.getModDir() / "baselib" / BaseLibFileName;
        auto baseLibContent = ll::file_utils::readFile(baseLibPath);
        if (!baseLibContent) {
            return ll::makeStringError(fmt::format("failed to read BaseLib at {}", baseLibPath.string()));
        }
        scriptEngine.eval(baseLibContent.value());
#endif
        // Load the plugin entry.
        auto pluginDir                         = std::filesystem::canonical(ll::mod::getModsRoot() / manifest.name);
        auto entryPath                         = pluginDir / manifest.entry;
        ENGINE_OWN_DATA()->pluginFileOrDirPath = ll::string_utils::u8str2str(entryPath.u8string());
#ifdef LEGACY_SCRIPT_ENGINE_BACKEND_PYTHON
        if (!PythonHelper::loadPluginCode(
                &scriptEngine,
                ll::string_utils::u8str2str(entryPath.u8string()),
                ll::string_utils::u8str2str(dirPath.u8string())
            )) {
            return ll::makeStringError(fmt::format("failed to load plugin code"));
        }
#endif
#ifdef LEGACY_SCRIPT_ENGINE_BACKEND_NODEJS
        if (!NodeJsHelper::loadPluginCode(
                &scriptEngine,
                ll::string_utils::u8str2str(entryPath.u8string()),
                ll::string_utils::u8str2str(dirPath.u8string())
            )) {
            return ll::makeStringError(fmt::format("failed to load plugin code"));
        }
#endif
#if (defined LEGACY_SCRIPT_ENGINE_BACKEND_QUICKJS) || (defined LEGACY_SCRIPT_ENGINE_BACKEND_LUA)
        // Try loadFile
        try {
            scriptEngine.loadFile(entryPath.u8string());
        } catch (const script::Exception& e) {
            // loadFile failed, try eval
            auto pluginEntryContent = ll::file_utils::readFile(entryPath);
            if (!pluginEntryContent) {
                return ll::makeStringError(fmt::format("Failed to read plugin entry at {}", entryPath.string()));
            }
            scriptEngine.eval(pluginEntryContent.value(), entryPath.u8string());
        }
        if (ll::getServerStatus() == ll::ServerStatus::Running) { // Is hot load
            LLSECallEventsOnHotLoad(&scriptEngine);
        }
        ExitEngineScope exit;
#endif
        plugin->onLoad([](ll::mod::Mod& plugin) { return true; });
        plugin->onUnload([](ll::mod::Mod& plugin) { return true; });
        plugin->onEnable([](ll::mod::Mod& plugin) { return true; });
        plugin->onDisable([](ll::mod::Mod& plugin) { return true; });
    } catch (const Exception& e) {
        EngineScope engineScope(scriptEngine);
        auto        error =
            ll::makeStringError("Failed to load plugin {0}: {1}\n{2}"_tr(manifest.name, e.message(), e.stacktrace()));
        ExitEngineScope exit;
        LLSERemoveTimeTaskData(&scriptEngine);
        LLSERemoveAllEventListeners(&scriptEngine);
        LLSERemoveCmdRegister(&scriptEngine);
        LLSERemoveCmdCallback(&scriptEngine);
        LLSERemoveAllExportedFuncs(&scriptEngine);

        scriptEngine.getData().reset();

        EngineManager::unregisterEngine(&scriptEngine);

        return error;
    }

    addMod(manifest.name, plugin);

    return {};
}

ll::Expected<> PluginManager::unload(std::string_view name) {
    auto& logger = getSelfPluginInstance().getLogger();

    try {

        auto plugin = std::static_pointer_cast<Plugin>(getMod(name));

        logger.info("Unloading plugin {}", name);

        auto& scriptEngine = *EngineManager::getEngine(std::string(name));

#ifndef LEGACY_SCRIPT_ENGINE_BACKEND_NODEJS
        LLSERemoveTimeTaskData(&scriptEngine);
#endif
        LLSERemoveAllEventListeners(&scriptEngine);
        LLSERemoveCmdRegister(&scriptEngine);
        LLSERemoveCmdCallback(&scriptEngine);
        LLSERemoveAllExportedFuncs(&scriptEngine);

        scriptEngine.getData().reset();
        EngineManager::unregisterEngine(&scriptEngine);
#ifdef LEGACY_SCRIPT_ENGINE_BACKEND_NODEJS
        NodeJsHelper::stopEngine(&scriptEngine);
#else
        scriptEngine.destroy(); // TODO: use unique_ptr to manage the engine.
#endif
        eraseMod(name);

        return {};
    } catch (const std::exception& e) {
        return ll::makeStringError("Failed to unload plugin {}: {}"_tr(name, e.what()));
    }
}

} // namespace lse
