/*
 * DISTRHO Cardinal Plugin
 * Copyright (C) 2021-2026 Filipe Coelho <falktx@falktx.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "CardinalOSC.hpp"

#include "CardinalCommon.hpp"
#include "CardinalPluginContext.hpp"

#include <app/CableWidget.hpp>
#include <app/ModuleWidget.hpp>
#include <app/RackWidget.hpp>
#include <app/Scene.hpp>
#include <app/common.hpp>
#include <context.hpp>
#include <helpers.hpp>
#include <jansson.h>
#include <patch.hpp>
#include <plugin.hpp>
#include <settings.hpp>
#include <string.hpp>
#include <system.hpp>
#include <engine/Engine.hpp>

#include <cstdlib>

#ifndef DISTRHO_PLUGIN_WANT_DIRECT_ACCESS
# error wrong build
#endif

#if (defined(STATIC_BUILD) && !defined(__MOD_DEVICES__)) || CARDINAL_VARIANT_MINI
# undef CARDINAL_INIT_OSC_THREAD
#endif

#ifdef HAVE_LIBLO
# include <lo/lo.h>
#endif

START_NAMESPACE_DISTRHO

#ifdef HAVE_LIBLO
static void osc_error_handler(int num, const char* msg, const char* path)
{
    d_stderr("Cardinal OSC Error: code: %i, msg: \"%s\", path: \"%s\")", num, msg, path);
}

static int osc_fallback_handler(const char* const path, const char* const types, lo_arg**, int, lo_message, void*)
{
    d_stderr("Cardinal OSC unhandled message \"%s\" with types \"%s\"", path, types);
    return 0;
}

#ifdef CARDINAL_INIT_OSC_THREAD
static const char* oscFeatures = ":screenshot:";
#else
static const char* oscFeatures = "";
#endif

static int osc_hello_handler(const char*, const char*, lo_arg**, int, const lo_message m, void* const self)
{
    d_stdout("Hello received from OSC, saying hello back to them o/");
    const lo_address source = lo_message_get_source(m);
    const lo_server server = static_cast<Initializer*>(self)->oscServer;

    // send list of features first and then hello!
    lo_send_from(source, server, LO_TT_IMMEDIATE, "/resp", "ss", "features", oscFeatures);
    lo_send_from(source, server, LO_TT_IMMEDIATE, "/resp", "ss", "hello", "ok");
    return 0;
}

static void osc_send_resp(const lo_message m, Initializer* const init, const char* const msg, const char* const payload)
{
    const lo_address source = lo_message_get_source(m);
    const lo_server server = init->oscServer;
    lo_send_from(source, server, LO_TT_IMMEDIATE, "/resp", "ss", msg, payload != nullptr ? payload : "");
}

static void osc_send_json(const lo_message m, Initializer* const init, const char* const msg, json_t* const rootJ)
{
    char* const json = json_dumps(rootJ, JSON_COMPACT);
    if (json == nullptr)
    {
        osc_send_resp(m, init, msg, "[]");
        return;
    }
    osc_send_resp(m, init, msg, json);
    std::free(json);
}

static void osc_send_error(const lo_message m, Initializer* const init, const char* const msg, const char* const error)
{
    json_t* const rootJ = json_object();
    json_object_set_new(rootJ, "ok", json_false());
    json_object_set_new(rootJ, "error", json_string(error != nullptr ? error : "unknown"));
    osc_send_json(m, init, msg, rootJ);
    json_decref(rootJ);
}

static int osc_modules_list_handler(const char*, const char*, lo_arg**, int, const lo_message m, void* const self)
{
    d_debug("osc_modules_list_handler()");

    Initializer* const init = static_cast<Initializer*>(self);
    CardinalBasePlugin* const plugin = init->remotePluginInstance;
    if (plugin == nullptr)
    {
        osc_send_error(m, init, "modules", "no-plugin-instance");
        return 0;
    }

    CardinalPluginContext* const context = plugin->context;

   #ifdef CARDINAL_INIT_OSC_THREAD
    rack::contextSet(context);
   #endif

    const std::vector<int64_t> moduleIds = context->engine->getModuleIds();
    int total = 0;
    for (const int64_t moduleId : moduleIds)
    {
        rack::engine::Module* const module = context->engine->getModule(moduleId);
        if (module == nullptr || module->model == nullptr || module->model->plugin == nullptr)
            continue;
        ++total;
    }

    int index = 0;
    for (const int64_t moduleId : moduleIds)
    {
        rack::engine::Module* const module = context->engine->getModule(moduleId);
        if (module == nullptr || module->model == nullptr || module->model->plugin == nullptr)
            continue;

        json_t* const rootJ = json_object();
        json_t* const moduleJ = json_object();

        json_object_set_new(rootJ, "ok", json_true());
        json_object_set_new(rootJ, "index", json_integer(index));
        json_object_set_new(rootJ, "total", json_integer(total));
        json_object_set_new(rootJ, "module", moduleJ);

        json_object_set_new(moduleJ, "id", json_integer(moduleId));
        json_object_set_new(moduleJ, "plugin", json_string(module->model->plugin->slug.c_str()));
        json_object_set_new(moduleJ, "model", json_string(module->model->slug.c_str()));
        json_object_set_new(moduleJ, "name", json_string(module->model->name.c_str()));
        json_object_set_new(moduleJ, "fullName", json_string(module->model->getFullName().c_str()));

        osc_send_json(m, init, "modules", rootJ);
        json_decref(rootJ);
        ++index;
    }

   #ifdef CARDINAL_INIT_OSC_THREAD
    rack::contextSet(nullptr);
   #endif

    return 0;
}

static int osc_modules_available_handler(const char*, const char*, lo_arg**, int, const lo_message m, void* const self)
{
    d_debug("osc_modules_available_handler()");

    Initializer* const init = static_cast<Initializer*>(self);

    int total = 0;
    for (rack::plugin::Plugin* const plugin : rack::plugin::plugins)
    {
        if (plugin == nullptr)
            continue;
        for (rack::plugin::Model* const model : plugin->models)
        {
            if (model == nullptr || model->hidden)
                continue;
            ++total;
        }
    }

    int index = 0;
    for (rack::plugin::Plugin* const plugin : rack::plugin::plugins)
    {
        if (plugin == nullptr)
            continue;

        for (rack::plugin::Model* const model : plugin->models)
        {
            if (model == nullptr || model->hidden)
                continue;

            json_t* const rootJ = json_object();
            json_t* const moduleJ = json_object();

            json_object_set_new(rootJ, "ok", json_true());
            json_object_set_new(rootJ, "index", json_integer(index));
            json_object_set_new(rootJ, "total", json_integer(total));
            json_object_set_new(rootJ, "module", moduleJ);

            json_object_set_new(moduleJ, "plugin", json_string(plugin->slug.c_str()));
            json_object_set_new(moduleJ, "model", json_string(model->slug.c_str()));
            json_object_set_new(moduleJ, "name", json_string(model->name.c_str()));
            json_object_set_new(moduleJ, "fullName", json_string(model->getFullName().c_str()));

            osc_send_json(m, init, "modules-available", rootJ);
            json_decref(rootJ);
            ++index;
        }
    }

    return 0;
}

static int osc_module_add_common(const lo_message m, void* const self,
                                 const char* const pluginSlug, const char* const modelSlug,
                                 const bool hasPos, const int posX, const int posY)
{
    d_debug("osc_module_add_common()");

    Initializer* const init = static_cast<Initializer*>(self);
    CardinalBasePlugin* const plugin = init->remotePluginInstance;
    if (plugin == nullptr)
    {
        osc_send_error(m, init, "module-add", "no-plugin-instance");
        return 0;
    }

    if (pluginSlug == nullptr || modelSlug == nullptr)
    {
        osc_send_error(m, init, "module-add", "missing-slug");
        return 0;
    }

    CardinalPluginContext* const context = plugin->context;

   #ifdef CARDINAL_INIT_OSC_THREAD
    rack::contextSet(context);
   #endif

    rack::plugin::Model* const model = rack::plugin::getModel(pluginSlug, modelSlug);
    if (model == nullptr)
    {
        osc_send_error(m, init, "module-add", "model-not-found");
       #ifdef CARDINAL_INIT_OSC_THREAD
        rack::contextSet(nullptr);
       #endif
        return 0;
    }

    rack::engine::Module* const module = model->createModule();
    if (module == nullptr)
    {
        osc_send_error(m, init, "module-add", "module-create-failed");
       #ifdef CARDINAL_INIT_OSC_THREAD
        rack::contextSet(nullptr);
       #endif
        return 0;
    }

    context->engine->addModule(module);

   #ifndef HEADLESS
    rack::app::ModuleWidget* const moduleWidget = model->createModuleWidget(module);
    if (moduleWidget == nullptr)
    {
        context->engine->removeModule(module);
        delete module;
        osc_send_error(m, init, "module-add", "module-widget-create-failed");
       #ifdef CARDINAL_INIT_OSC_THREAD
        rack::contextSet(nullptr);
       #endif
        return 0;
    }
    if (context->scene == nullptr || context->scene->rack == nullptr)
    {
        delete moduleWidget;
        osc_send_error(m, init, "module-add", "no-rack-scene");
       #ifdef CARDINAL_INIT_OSC_THREAD
        rack::contextSet(nullptr);
       #endif
        return 0;
    }
    if (hasPos)
    {
        moduleWidget->setGridPosition(rack::math::Vec(posX, posY));
        context->scene->rack->addModule(moduleWidget);
    }
    else
    {
        const rack::math::Vec pos = rack::app::RACK_OFFSET;
        if (rack::settings::squeezeModules)
            context->scene->rack->setModulePosSqueeze(moduleWidget, pos);
        else
            context->scene->rack->setModulePosNearest(moduleWidget, pos);
        context->scene->rack->addModule(moduleWidget);
    }
    moduleWidget->loadTemplate();
   #endif

    json_t* const rootJ = json_object();
    json_object_set_new(rootJ, "ok", json_true());
    json_object_set_new(rootJ, "id", json_integer(module->id));
    json_object_set_new(rootJ, "plugin", json_string(pluginSlug));
    json_object_set_new(rootJ, "model", json_string(modelSlug));
    osc_send_json(m, init, "module-add", rootJ);
    json_decref(rootJ);

   #ifdef CARDINAL_INIT_OSC_THREAD
    rack::contextSet(nullptr);
   #endif

    return 0;
}

static int osc_module_add_handler(const char*, const char* types, lo_arg** argv, int argc, const lo_message m, void* const self)
{
    d_debug("osc_module_add_handler()");
    DISTRHO_SAFE_ASSERT_RETURN(argc == 2, 0);
    DISTRHO_SAFE_ASSERT_RETURN(types != nullptr, 0);
    DISTRHO_SAFE_ASSERT_RETURN(types[0] == 's', 0);
    DISTRHO_SAFE_ASSERT_RETURN(types[1] == 's', 0);

    const char* const pluginSlug = &argv[0]->s;
    const char* const modelSlug = &argv[1]->s;
    return osc_module_add_common(m, self, pluginSlug, modelSlug, false, 0, 0);
}

static int osc_module_add_pos_handler(const char*, const char* types, lo_arg** argv, int argc, const lo_message m, void* const self)
{
    d_debug("osc_module_add_pos_handler()");
    DISTRHO_SAFE_ASSERT_RETURN(argc == 4, 0);
    DISTRHO_SAFE_ASSERT_RETURN(types != nullptr, 0);
    DISTRHO_SAFE_ASSERT_RETURN(types[0] == 's', 0);
    DISTRHO_SAFE_ASSERT_RETURN(types[1] == 's', 0);
    DISTRHO_SAFE_ASSERT_RETURN(types[2] == 'i', 0);
    DISTRHO_SAFE_ASSERT_RETURN(types[3] == 'i', 0);

    const char* const pluginSlug = &argv[0]->s;
    const char* const modelSlug = &argv[1]->s;
    const int posX = argv[2]->i;
    const int posY = argv[3]->i;
    return osc_module_add_common(m, self, pluginSlug, modelSlug, true, posX, posY);
}

static int osc_module_remove_handler(const char*, const char* types, lo_arg** argv, int argc, const lo_message m, void* const self)
{
    d_debug("osc_module_remove_handler()");
    DISTRHO_SAFE_ASSERT_RETURN(argc == 1, 0);
    DISTRHO_SAFE_ASSERT_RETURN(types != nullptr, 0);
    DISTRHO_SAFE_ASSERT_RETURN(types[0] == 'h', 0);

    Initializer* const init = static_cast<Initializer*>(self);
    CardinalBasePlugin* const plugin = init->remotePluginInstance;
    if (plugin == nullptr)
    {
        osc_send_error(m, init, "module-remove", "no-plugin-instance");
        return 0;
    }

    CardinalPluginContext* const context = plugin->context;
    const int64_t moduleId = argv[0]->h;

   #ifdef CARDINAL_INIT_OSC_THREAD
    rack::contextSet(context);
   #endif

   #ifndef HEADLESS
    if (context->scene != nullptr && context->scene->rack != nullptr)
    {
        rack::app::ModuleWidget* const mw = context->scene->rack->getModule(moduleId);
        if (mw == nullptr)
        {
            osc_send_error(m, init, "module-remove", "module-not-found");
           #ifdef CARDINAL_INIT_OSC_THREAD
            rack::contextSet(nullptr);
           #endif
            return 0;
        }
        context->scene->rack->removeModule(mw);
        delete mw;
    }
    else
   #endif
    {
        rack::engine::Module* const module = context->engine->getModule(moduleId);
        if (module == nullptr)
        {
            osc_send_error(m, init, "module-remove", "module-not-found");
           #ifdef CARDINAL_INIT_OSC_THREAD
            rack::contextSet(nullptr);
           #endif
            return 0;
        }
        // Disconnect cables in headless mode to avoid engine asserts.
        const std::vector<int64_t> cableIds = context->engine->getCableIds();
        for (const int64_t cableId : cableIds)
        {
            rack::engine::Cable* const cable = context->engine->getCable(cableId);
            if (cable == nullptr)
                continue;
            if (cable->inputModule == module || cable->outputModule == module)
            {
                context->engine->removeCable(cable);
                delete cable;
            }
        }
        context->engine->removeModule(module);
        delete module;
    }

    json_t* const rootJ = json_object();
    json_object_set_new(rootJ, "ok", json_true());
    json_object_set_new(rootJ, "id", json_integer(moduleId));
    osc_send_json(m, init, "module-remove", rootJ);
    json_decref(rootJ);

   #ifdef CARDINAL_INIT_OSC_THREAD
    rack::contextSet(nullptr);
   #endif

    return 0;
}

static int osc_module_info_handler(const char*, const char* types, lo_arg** argv, int argc, const lo_message m, void* const self)
{
    d_debug("osc_module_info_handler()");
    DISTRHO_SAFE_ASSERT_RETURN(argc == 1, 0);
    DISTRHO_SAFE_ASSERT_RETURN(types != nullptr, 0);
    DISTRHO_SAFE_ASSERT_RETURN(types[0] == 'h', 0);

    Initializer* const init = static_cast<Initializer*>(self);
    CardinalBasePlugin* const plugin = init->remotePluginInstance;
    if (plugin == nullptr)
    {
        osc_send_error(m, init, "module-info", "no-plugin-instance");
        return 0;
    }

    CardinalPluginContext* const context = plugin->context;
    const int64_t moduleId = argv[0]->h;

   #ifdef CARDINAL_INIT_OSC_THREAD
    rack::contextSet(context);
   #endif

    rack::engine::Module* const module = context->engine->getModule(moduleId);
    if (module == nullptr || module->model == nullptr || module->model->plugin == nullptr)
    {
        osc_send_error(m, init, "module-info", "module-not-found");
       #ifdef CARDINAL_INIT_OSC_THREAD
        rack::contextSet(nullptr);
       #endif
        return 0;
    }

    json_t* const rootJ = json_object();
    json_t* const moduleJ = json_object();
    json_object_set_new(rootJ, "ok", json_true());
    json_object_set_new(rootJ, "module", moduleJ);

    json_object_set_new(moduleJ, "id", json_integer(moduleId));
    json_object_set_new(moduleJ, "plugin", json_string(module->model->plugin->slug.c_str()));
    json_object_set_new(moduleJ, "model", json_string(module->model->slug.c_str()));
    json_object_set_new(moduleJ, "name", json_string(module->model->name.c_str()));
    json_object_set_new(moduleJ, "fullName", json_string(module->model->getFullName().c_str()));
    json_object_set_new(moduleJ, "paramCount", json_integer(module->getNumParams()));
    json_object_set_new(moduleJ, "inputCount", json_integer(module->getNumInputs()));
    json_object_set_new(moduleJ, "outputCount", json_integer(module->getNumOutputs()));
    json_object_set_new(moduleJ, "lightCount", json_integer(module->getNumLights()));

   #ifndef HEADLESS
    if (context->scene != nullptr && context->scene->rack != nullptr)
    {
        if (rack::app::ModuleWidget* const mw = context->scene->rack->getModule(moduleId))
        {
            const rack::math::Vec gridPos = mw->getGridPosition();
            json_t* const posJ = json_pack("[i, i]", static_cast<int>(gridPos.x), static_cast<int>(gridPos.y));
            json_object_set_new(moduleJ, "pos", posJ);
        }
    }
   #endif

    osc_send_json(m, init, "module-info", rootJ);
    json_decref(rootJ);

   #ifdef CARDINAL_INIT_OSC_THREAD
    rack::contextSet(nullptr);
   #endif

    return 0;
}

static int osc_module_params_handler(const char*, const char* types, lo_arg** argv, int argc, const lo_message m, void* const self)
{
    d_debug("osc_module_params_handler()");
    DISTRHO_SAFE_ASSERT_RETURN(argc == 1, 0);
    DISTRHO_SAFE_ASSERT_RETURN(types != nullptr, 0);
    DISTRHO_SAFE_ASSERT_RETURN(types[0] == 'h', 0);

    Initializer* const init = static_cast<Initializer*>(self);
    CardinalBasePlugin* const plugin = init->remotePluginInstance;
    if (plugin == nullptr)
    {
        osc_send_error(m, init, "module-params", "no-plugin-instance");
        return 0;
    }

    CardinalPluginContext* const context = plugin->context;
    const int64_t moduleId = argv[0]->h;

   #ifdef CARDINAL_INIT_OSC_THREAD
    rack::contextSet(context);
   #endif

    rack::engine::Module* const module = context->engine->getModule(moduleId);
    if (module == nullptr)
    {
        osc_send_error(m, init, "module-params", "module-not-found");
       #ifdef CARDINAL_INIT_OSC_THREAD
        rack::contextSet(nullptr);
       #endif
        return 0;
    }

    const int numParams = module->getNumParams();
    if (numParams == 0)
    {
        json_t* const rootJ = json_object();
        json_object_set_new(rootJ, "ok", json_true());
        json_object_set_new(rootJ, "index", json_integer(0));
        json_object_set_new(rootJ, "total", json_integer(0));
        json_object_set_new(rootJ, "id", json_integer(moduleId));
        json_object_set_new(rootJ, "param", json_null());
        osc_send_json(m, init, "module-params", rootJ);
        json_decref(rootJ);
    }
    for (int paramId = 0; paramId < numParams; ++paramId)
    {
        json_t* const rootJ = json_object();
        json_t* const paramJ = json_object();
        json_object_set_new(rootJ, "ok", json_true());
        json_object_set_new(rootJ, "index", json_integer(paramId));
        json_object_set_new(rootJ, "total", json_integer(numParams));
        json_object_set_new(rootJ, "id", json_integer(moduleId));
        json_object_set_new(rootJ, "param", paramJ);

        json_object_set_new(paramJ, "id", json_integer(paramId));
        json_object_set_new(paramJ, "value", json_real(context->engine->getParamValue(module, paramId)));

        if (rack::engine::ParamQuantity* const pq = module->getParamQuantity(paramId))
        {
            if (! pq->name.empty())
                json_object_set_new(paramJ, "name", json_string(pq->name.c_str()));
            if (! pq->unit.empty())
                json_object_set_new(paramJ, "unit", json_string(pq->unit.c_str()));
            if (! pq->description.empty())
                json_object_set_new(paramJ, "description", json_string(pq->description.c_str()));

            json_object_set_new(paramJ, "min", json_real(pq->minValue));
            json_object_set_new(paramJ, "max", json_real(pq->maxValue));
            json_object_set_new(paramJ, "default", json_real(pq->defaultValue));
        }
        osc_send_json(m, init, "module-params", rootJ);
        json_decref(rootJ);
    }

   #ifdef CARDINAL_INIT_OSC_THREAD
    rack::contextSet(nullptr);
   #endif

    return 0;
}

static int osc_module_inputs_handler(const char*, const char* types, lo_arg** argv, int argc, const lo_message m, void* const self)
{
    d_debug("osc_module_inputs_handler()");
    DISTRHO_SAFE_ASSERT_RETURN(argc == 1, 0);
    DISTRHO_SAFE_ASSERT_RETURN(types != nullptr, 0);
    DISTRHO_SAFE_ASSERT_RETURN(types[0] == 'h', 0);

    Initializer* const init = static_cast<Initializer*>(self);
    CardinalBasePlugin* const plugin = init->remotePluginInstance;
    if (plugin == nullptr)
    {
        osc_send_error(m, init, "module-inputs", "no-plugin-instance");
        return 0;
    }

    CardinalPluginContext* const context = plugin->context;
    const int64_t moduleId = argv[0]->h;

   #ifdef CARDINAL_INIT_OSC_THREAD
    rack::contextSet(context);
   #endif

    rack::engine::Module* const module = context->engine->getModule(moduleId);
    if (module == nullptr)
    {
        osc_send_error(m, init, "module-inputs", "module-not-found");
       #ifdef CARDINAL_INIT_OSC_THREAD
        rack::contextSet(nullptr);
       #endif
        return 0;
    }

    const int numInputs = module->getNumInputs();
    if (numInputs == 0)
    {
        json_t* const rootJ = json_object();
        json_object_set_new(rootJ, "ok", json_true());
        json_object_set_new(rootJ, "index", json_integer(0));
        json_object_set_new(rootJ, "total", json_integer(0));
        json_object_set_new(rootJ, "id", json_integer(moduleId));
        json_object_set_new(rootJ, "input", json_null());
        osc_send_json(m, init, "module-inputs", rootJ);
        json_decref(rootJ);
    }
    for (int portId = 0; portId < numInputs; ++portId)
    {
        json_t* const rootJ = json_object();
        json_t* const inputJ = json_object();
        json_object_set_new(rootJ, "ok", json_true());
        json_object_set_new(rootJ, "index", json_integer(portId));
        json_object_set_new(rootJ, "total", json_integer(numInputs));
        json_object_set_new(rootJ, "id", json_integer(moduleId));
        json_object_set_new(rootJ, "input", inputJ);

        json_object_set_new(inputJ, "id", json_integer(portId));
        if (rack::engine::PortInfo* const info = module->getInputInfo(portId))
        {
            if (! info->name.empty())
                json_object_set_new(inputJ, "name", json_string(info->name.c_str()));
            if (! info->description.empty())
                json_object_set_new(inputJ, "description", json_string(info->description.c_str()));
            const std::string fullName = info->getFullName();
            if (! fullName.empty())
                json_object_set_new(inputJ, "fullName", json_string(fullName.c_str()));
        }

        osc_send_json(m, init, "module-inputs", rootJ);
        json_decref(rootJ);
    }

   #ifdef CARDINAL_INIT_OSC_THREAD
    rack::contextSet(nullptr);
   #endif

    return 0;
}

static int osc_module_outputs_handler(const char*, const char* types, lo_arg** argv, int argc, const lo_message m, void* const self)
{
    d_debug("osc_module_outputs_handler()");
    DISTRHO_SAFE_ASSERT_RETURN(argc == 1, 0);
    DISTRHO_SAFE_ASSERT_RETURN(types != nullptr, 0);
    DISTRHO_SAFE_ASSERT_RETURN(types[0] == 'h', 0);

    Initializer* const init = static_cast<Initializer*>(self);
    CardinalBasePlugin* const plugin = init->remotePluginInstance;
    if (plugin == nullptr)
    {
        osc_send_error(m, init, "module-outputs", "no-plugin-instance");
        return 0;
    }

    CardinalPluginContext* const context = plugin->context;
    const int64_t moduleId = argv[0]->h;

   #ifdef CARDINAL_INIT_OSC_THREAD
    rack::contextSet(context);
   #endif

    rack::engine::Module* const module = context->engine->getModule(moduleId);
    if (module == nullptr)
    {
        osc_send_error(m, init, "module-outputs", "module-not-found");
       #ifdef CARDINAL_INIT_OSC_THREAD
        rack::contextSet(nullptr);
       #endif
        return 0;
    }

    const int numOutputs = module->getNumOutputs();
    if (numOutputs == 0)
    {
        json_t* const rootJ = json_object();
        json_object_set_new(rootJ, "ok", json_true());
        json_object_set_new(rootJ, "index", json_integer(0));
        json_object_set_new(rootJ, "total", json_integer(0));
        json_object_set_new(rootJ, "id", json_integer(moduleId));
        json_object_set_new(rootJ, "output", json_null());
        osc_send_json(m, init, "module-outputs", rootJ);
        json_decref(rootJ);
    }
    for (int portId = 0; portId < numOutputs; ++portId)
    {
        json_t* const rootJ = json_object();
        json_t* const outputJ = json_object();
        json_object_set_new(rootJ, "ok", json_true());
        json_object_set_new(rootJ, "index", json_integer(portId));
        json_object_set_new(rootJ, "total", json_integer(numOutputs));
        json_object_set_new(rootJ, "id", json_integer(moduleId));
        json_object_set_new(rootJ, "output", outputJ);

        json_object_set_new(outputJ, "id", json_integer(portId));
        if (rack::engine::PortInfo* const info = module->getOutputInfo(portId))
        {
            if (! info->name.empty())
                json_object_set_new(outputJ, "name", json_string(info->name.c_str()));
            if (! info->description.empty())
                json_object_set_new(outputJ, "description", json_string(info->description.c_str()));
            const std::string fullName = info->getFullName();
            if (! fullName.empty())
                json_object_set_new(outputJ, "fullName", json_string(fullName.c_str()));
        }

        osc_send_json(m, init, "module-outputs", rootJ);
        json_decref(rootJ);
    }

   #ifdef CARDINAL_INIT_OSC_THREAD
    rack::contextSet(nullptr);
   #endif

    return 0;
}

static int osc_cables_list_handler(const char*, const char* types, lo_arg** argv, int argc, const lo_message m, void* const self)
{
    d_debug("osc_cables_list_handler()");
    DISTRHO_SAFE_ASSERT_RETURN(argc == 0 || argc == 1, 0);
    if (argc == 1)
    {
        DISTRHO_SAFE_ASSERT_RETURN(types != nullptr, 0);
        DISTRHO_SAFE_ASSERT_RETURN(types[0] == 'h', 0);
    }

    Initializer* const init = static_cast<Initializer*>(self);
    CardinalBasePlugin* const plugin = init->remotePluginInstance;
    if (plugin == nullptr)
    {
        osc_send_error(m, init, "cables", "no-plugin-instance");
        return 0;
    }

    CardinalPluginContext* const context = plugin->context;
    const int64_t moduleFilter = (argc == 1) ? argv[0]->h : -1;

   #ifdef CARDINAL_INIT_OSC_THREAD
    rack::contextSet(context);
   #endif

    const std::vector<int64_t> cableIds = context->engine->getCableIds();
    int total = 0;
    for (const int64_t cableId : cableIds)
    {
        rack::engine::Cable* const cable = context->engine->getCable(cableId);
        if (cable == nullptr || cable->inputModule == nullptr || cable->outputModule == nullptr)
            continue;
        if (moduleFilter >= 0
            && cable->inputModule->id != moduleFilter
            && cable->outputModule->id != moduleFilter)
        {
            continue;
        }
        ++total;
    }

    if (total == 0)
    {
        json_t* const rootJ = json_object();
        json_object_set_new(rootJ, "ok", json_true());
        json_object_set_new(rootJ, "index", json_integer(0));
        json_object_set_new(rootJ, "total", json_integer(0));
        json_object_set_new(rootJ, "cable", json_null());
        if (moduleFilter >= 0)
            json_object_set_new(rootJ, "moduleId", json_integer(moduleFilter));
        osc_send_json(m, init, "cables", rootJ);
        json_decref(rootJ);
    }

    int index = 0;
    for (const int64_t cableId : cableIds)
    {
        rack::engine::Cable* const cable = context->engine->getCable(cableId);
        if (cable == nullptr || cable->inputModule == nullptr || cable->outputModule == nullptr)
            continue;
        if (moduleFilter >= 0
            && cable->inputModule->id != moduleFilter
            && cable->outputModule->id != moduleFilter)
        {
            continue;
        }

        json_t* const rootJ = json_object();
        json_t* const cableJ = json_object();
        json_object_set_new(rootJ, "ok", json_true());
        json_object_set_new(rootJ, "index", json_integer(index));
        json_object_set_new(rootJ, "total", json_integer(total));
        json_object_set_new(rootJ, "cable", cableJ);
        if (moduleFilter >= 0)
            json_object_set_new(rootJ, "moduleId", json_integer(moduleFilter));

        json_object_set_new(cableJ, "id", json_integer(cable->id));
        json_object_set_new(cableJ, "outputModuleId", json_integer(cable->outputModule->id));
        json_object_set_new(cableJ, "outputId", json_integer(cable->outputId));
        json_object_set_new(cableJ, "inputModuleId", json_integer(cable->inputModule->id));
        json_object_set_new(cableJ, "inputId", json_integer(cable->inputId));

        osc_send_json(m, init, "cables", rootJ);
        json_decref(rootJ);
        ++index;
    }

   #ifdef CARDINAL_INIT_OSC_THREAD
    rack::contextSet(nullptr);
   #endif

    return 0;
}

static int osc_cable_add_handler(const char*, const char* types, lo_arg** argv, int argc, const lo_message m, void* const self)
{

    Initializer* const init = static_cast<Initializer*>(self);
    CardinalBasePlugin* const plugin = init->remotePluginInstance;
    if (plugin == nullptr)
    {
        osc_send_error(m, init, "cable-add", "no-plugin-instance");
        return 0;
    }

    CardinalPluginContext* const context = plugin->context;
    const int64_t outputModuleId = argv[0]->h;
    const int outputId = argv[1]->i;
    const int64_t inputModuleId = argv[2]->h;
    const int inputId = argv[3]->i;

   #ifdef CARDINAL_INIT_OSC_THREAD
    rack::contextSet(context);
   #endif

    rack::engine::Module* const outputModule = context->engine->getModule(outputModuleId);
    rack::engine::Module* const inputModule = context->engine->getModule(inputModuleId);
    if (outputModule == nullptr || inputModule == nullptr)
    {
        osc_send_error(m, init, "cable-add", "module-not-found");
       #ifdef CARDINAL_INIT_OSC_THREAD
        rack::contextSet(nullptr);
       #endif
        return 0;
    }
    if (outputId < 0 || outputId >= outputModule->getNumOutputs())
    {
        osc_send_error(m, init, "cable-add", "output-not-found");
       #ifdef CARDINAL_INIT_OSC_THREAD
        rack::contextSet(nullptr);
       #endif
        return 0;
    }
    if (inputId < 0 || inputId >= inputModule->getNumInputs())
    {
        osc_send_error(m, init, "cable-add", "input-not-found");
       #ifdef CARDINAL_INIT_OSC_THREAD
        rack::contextSet(nullptr);
       #endif
        return 0;
    }

    const std::vector<int64_t> cableIds = context->engine->getCableIds();
    for (const int64_t cableId : cableIds)
    {
        rack::engine::Cable* const cable = context->engine->getCable(cableId);
        if (cable == nullptr)
            continue;
        if (cable->inputModule == inputModule && cable->inputId == inputId)
        {
            osc_send_error(m, init, "cable-add", "input-already-connected");
           #ifdef CARDINAL_INIT_OSC_THREAD
            rack::contextSet(nullptr);
           #endif
            return 0;
        }
    }

    rack::engine::Cable* const cable = new rack::engine::Cable;
    cable->outputModule = outputModule;
    cable->outputId = outputId;
    cable->inputModule = inputModule;
    cable->inputId = inputId;
    context->engine->addCable(cable);

   #ifndef HEADLESS
    if (context->scene != nullptr && context->scene->rack != nullptr)
    {
        try {
            rack::app::CableWidget* const cw = new rack::app::CableWidget;
            cw->color = context->scene->rack->getNextCableColor();
            cw->setCable(cable);
            context->scene->rack->addCable(cw);
        }
        catch (rack::Exception&)
        {
            context->engine->removeCable(cable);
            delete cable;
            osc_send_error(m, init, "cable-add", "cable-widget-create-failed");
           #ifdef CARDINAL_INIT_OSC_THREAD
            rack::contextSet(nullptr);
           #endif
            return 0;
        }
    }
   #endif

    json_t* const rootJ = json_object();
    json_object_set_new(rootJ, "ok", json_true());
    json_object_set_new(rootJ, "id", json_integer(cable->id));
    json_object_set_new(rootJ, "outputModuleId", json_integer(outputModuleId));
    json_object_set_new(rootJ, "outputId", json_integer(outputId));
    json_object_set_new(rootJ, "inputModuleId", json_integer(inputModuleId));
    json_object_set_new(rootJ, "inputId", json_integer(inputId));
    osc_send_json(m, init, "cable-add", rootJ);
    json_decref(rootJ);

   #ifdef CARDINAL_INIT_OSC_THREAD
    rack::contextSet(nullptr);
   #endif

    return 0;
}

static int osc_cable_remove_handler(const char*, const char* types, lo_arg** argv, int argc, const lo_message m, void* const self)
{

    Initializer* const init = static_cast<Initializer*>(self);
    CardinalBasePlugin* const plugin = init->remotePluginInstance;
    if (plugin == nullptr)
    {
        osc_send_error(m, init, "cable-remove", "no-plugin-instance");
        return 0;
    }

    CardinalPluginContext* const context = plugin->context;
    const int64_t cableId = argv[0]->h;

   #ifdef CARDINAL_INIT_OSC_THREAD
    rack::contextSet(context);
   #endif

   #ifndef HEADLESS
    if (context->scene != nullptr && context->scene->rack != nullptr)
    {
        if (rack::app::CableWidget* const cw = context->scene->rack->getCable(cableId))
        {
            context->scene->rack->removeCable(cw);
            delete cw;
        }
        else
        {
            rack::engine::Cable* const cable = context->engine->getCable(cableId);
            if (cable == nullptr)
            {
                osc_send_error(m, init, "cable-remove", "cable-not-found");
               #ifdef CARDINAL_INIT_OSC_THREAD
                rack::contextSet(nullptr);
               #endif
                return 0;
            }
            context->engine->removeCable(cable);
            delete cable;
        }
    }
    else
   #endif
    {
        rack::engine::Cable* const cable = context->engine->getCable(cableId);
        if (cable == nullptr)
        {
            osc_send_error(m, init, "cable-remove", "cable-not-found");
           #ifdef CARDINAL_INIT_OSC_THREAD
            rack::contextSet(nullptr);
           #endif
            return 0;
        }
        context->engine->removeCable(cable);
        delete cable;
    }

    json_t* const rootJ = json_object();
    json_object_set_new(rootJ, "ok", json_true());
    json_object_set_new(rootJ, "id", json_integer(cableId));
    osc_send_json(m, init, "cable-remove", rootJ);
    json_decref(rootJ);

   #ifdef CARDINAL_INIT_OSC_THREAD
    rack::contextSet(nullptr);
   #endif

    return 0;
}

static int osc_load_handler(const char*, const char* types, lo_arg** argv, int argc, const lo_message m, void* const self)
{
    d_debug("osc_load_handler()");
    DISTRHO_SAFE_ASSERT_RETURN(argc == 1, 0);
    DISTRHO_SAFE_ASSERT_RETURN(types != nullptr && types[0] == 'b', 0);

    const int32_t size = argv[0]->blob.size;
    DISTRHO_SAFE_ASSERT_RETURN(size > 4, 0);

    const uint8_t* const blob = (uint8_t*)(&argv[0]->blob.data);
    DISTRHO_SAFE_ASSERT_RETURN(blob != nullptr, 0);

    bool ok = false;

    if (CardinalBasePlugin* const plugin = static_cast<Initializer*>(self)->remotePluginInstance)
    {
        CardinalPluginContext* const context = plugin->context;
        std::vector<uint8_t> data(size);
        std::memcpy(data.data(), blob, size);

       #ifdef CARDINAL_INIT_OSC_THREAD
        rack::contextSet(context);
       #endif

        rack::system::removeRecursively(context->patch->autosavePath);
        rack::system::createDirectories(context->patch->autosavePath);
        try {
            rack::system::unarchiveToDirectory(data, context->patch->autosavePath);
            context->patch->loadAutosave();
            ok = true;
        }
        catch (rack::Exception& e) {
            WARN("%s", e.what());
        }

       #ifdef CARDINAL_INIT_OSC_THREAD
        rack::contextSet(nullptr);
       #endif
    }

    const lo_address source = lo_message_get_source(m);
    const lo_server server = static_cast<Initializer*>(self)->oscServer;
    lo_send_from(source, server, LO_TT_IMMEDIATE, "/resp", "ss", "load", ok ? "ok" : "fail");
    return 0;
}

static int osc_param_handler(const char*, const char* types, lo_arg** argv, int argc, const lo_message m, void* const self)
{
    d_debug("osc_param_handler()");
    DISTRHO_SAFE_ASSERT_RETURN(argc == 3, 0);
    DISTRHO_SAFE_ASSERT_RETURN(types != nullptr, 0);
    DISTRHO_SAFE_ASSERT_RETURN(types[0] == 'h', 0);
    DISTRHO_SAFE_ASSERT_RETURN(types[1] == 'i', 0);
    DISTRHO_SAFE_ASSERT_RETURN(types[2] == 'f', 0);

    if (CardinalBasePlugin* const plugin = static_cast<Initializer*>(self)->remotePluginInstance)
    {
        CardinalPluginContext* const context = plugin->context;

        const int64_t moduleId = argv[0]->h;
        const int paramId = argv[1]->i;
        const float paramValue = argv[2]->f;

       #ifdef CARDINAL_INIT_OSC_THREAD
        rack::contextSet(context);
       #endif

        rack::engine::Module* const module = context->engine->getModule(moduleId);
        DISTRHO_SAFE_ASSERT_RETURN(module != nullptr, 0);

        context->engine->setParamValue(module, paramId, paramValue);

       #ifdef CARDINAL_INIT_OSC_THREAD
        rack::contextSet(nullptr);
       #endif
    }

    return 0;
}

static int osc_host_param_handler(const char*, const char* types, lo_arg** argv, int argc, const lo_message, void* const self)
{
    d_debug("osc_host_param_handler()");
    DISTRHO_SAFE_ASSERT_RETURN(argc == 2, 0);
    DISTRHO_SAFE_ASSERT_RETURN(types != nullptr, 0);
    DISTRHO_SAFE_ASSERT_RETURN(types[0] == 'i', 0);
    DISTRHO_SAFE_ASSERT_RETURN(types[1] == 'f', 0);

    if (CardinalBasePlugin* const plugin = static_cast<Initializer*>(self)->remotePluginInstance)
    {
        CardinalPluginContext* const context = plugin->context;

        const int paramId = argv[0]->i;
        DISTRHO_SAFE_ASSERT_RETURN(paramId >= 0, 0);

        const uint uparamId = static_cast<uint>(paramId);
        DISTRHO_SAFE_ASSERT_UINT2_RETURN(uparamId < kModuleParameterCount, uparamId, kModuleParameterCount, 0);

        const float paramValue = argv[1]->f;

        context->parameters[uparamId] = paramValue;
    }

    return 0;
}

# ifdef CARDINAL_INIT_OSC_THREAD
static int osc_screenshot_handler(const char*, const char* types, lo_arg** argv, int argc, const lo_message m, void* const self)
{
    d_debug("osc_screenshot_handler()");
    DISTRHO_SAFE_ASSERT_RETURN(argc == 1, 0);
    DISTRHO_SAFE_ASSERT_RETURN(types != nullptr && types[0] == 'b', 0);

    const int32_t size = argv[0]->blob.size;
    DISTRHO_SAFE_ASSERT_RETURN(size > 4, 0);

    const uint8_t* const blob = (uint8_t*)(&argv[0]->blob.data);
    DISTRHO_SAFE_ASSERT_RETURN(blob != nullptr, 0);

    bool ok = false;

    if (CardinalBasePlugin* const plugin = static_cast<Initializer*>(self)->remotePluginInstance)
    {
        if (char* const screenshot = String::asBase64(blob, size).getAndReleaseBuffer())
        {
            ok = plugin->updateStateValue("screenshot", screenshot);
            std::free(screenshot);
        }
    }

    const lo_address source = lo_message_get_source(m);
    const lo_server server = static_cast<Initializer*>(self)->oscServer;
    lo_send_from(source, server, LO_TT_IMMEDIATE, "/resp", "ss", "screenshot", ok ? "ok" : "fail");
    return 0;
}
# endif
#endif

void oscInitState(Initializer* const self)
{
#if defined(HAVE_LIBLO)
    const char* const portEnv = std::getenv("CARDINAL_REMOTE_HOST_PORT");
#if defined(CARDINAL_INIT_OSC_THREAD)
    // When in headless mode, always boot OSC in a thread with the default port
    // unless overridden.
    self->startRemoteServer(portEnv != nullptr ? portEnv : CARDINAL_DEFAULT_REMOTE_PORT);
#endif
    if (rack::isStandalone()) {
        if (portEnv != nullptr) {
            // If the envvar is set in headful modes, start now.
            if (!self->startRemoteServer(portEnv)) {
                WARN("Failed to start OSC Remote control on port %s", portEnv);
            }
        } else {
            INFO("OSC Remote control is available on request");
        }
    } else {
        INFO("OSC Remote control is not available on plugin variants");
    }
#else
    INFO("OSC Remote control is not enabled in this build");
    DISTRHO_UNUSED(self);
#endif
}

void oscShutdownState(Initializer* const self)
{
#ifdef HAVE_LIBLO
    self->stopRemoteServer();
#else
    DISTRHO_UNUSED(self);
#endif
}

#ifdef HAVE_LIBLO
struct OscMethod {
    const char* path;
    const char* types;
    lo_method_handler handler;
};

static const OscMethod kOscMethods[] = {
    {"/hello", "", osc_hello_handler},
    {"/host-param", "if", osc_host_param_handler},
    {"/load", "b", osc_load_handler},
    {"/param", "hif", osc_param_handler},
    {"/modules/list", "", osc_modules_list_handler},
    {"/modules/available", "", osc_modules_available_handler},
    {"/module/add", "ss", osc_module_add_handler},
    {"/module/add", "ssii", osc_module_add_pos_handler},
    {"/module/remove", "h", osc_module_remove_handler},
    {"/module/info", "h", osc_module_info_handler},
    {"/module/params", "h", osc_module_params_handler},
    {"/module/inputs", "h", osc_module_inputs_handler},
    {"/module/outputs", "h", osc_module_outputs_handler},
    {"/cables/list", "", osc_cables_list_handler},
    {"/cables/list", "h", osc_cables_list_handler},
    {"/cable/add", "hihi", osc_cable_add_handler},
    {"/cable/remove", "h", osc_cable_remove_handler},
   #ifdef CARDINAL_INIT_OSC_THREAD
    {"/screenshot", "b", osc_screenshot_handler},
   #endif
};

template <typename AddFn, typename Server>
static void addOscMethods(Server srv, AddFn addFn, void* self) {
    for (const auto& m : kOscMethods)
        addFn(srv, m.path, m.types, m.handler, self);
    addFn(srv, nullptr, nullptr, osc_fallback_handler, nullptr);
}

bool Initializer::startRemoteServer(const char* const port)
{
   #ifdef CARDINAL_INIT_OSC_THREAD
    if (oscServerThread != nullptr)
        return true;

    INFO("Starting OSC Remote control thread on %s (udp)", port);

    if ((oscServerThread = lo_server_thread_new_with_proto(port, LO_UDP, osc_error_handler)) == nullptr)
        return false;

    oscServer = lo_server_thread_get_server(oscServerThread);
    addOscMethods(oscServerThread, lo_server_thread_add_method, this);
    lo_server_thread_start(oscServerThread);
   #else
    if (oscServer != nullptr)
        return true;

    INFO("Starting OSC Remote control on %s (udp)", port);

    if ((oscServer = lo_server_new_with_proto(port, LO_UDP, osc_error_handler)) == nullptr)
        return false;

    addOscMethods(oscServer, lo_server_add_method, this);
   #endif

    return true;
}

void Initializer::stopRemoteServer()
{
    DISTRHO_SAFE_ASSERT(remotePluginInstance == nullptr);

   #ifdef CARDINAL_INIT_OSC_THREAD
    if (oscServerThread != nullptr)
    {
        lo_server_thread_stop(oscServerThread);
        lo_server_thread_del_method(oscServerThread, nullptr, nullptr);
        lo_server_thread_free(oscServerThread);
        oscServerThread = nullptr;
        oscServer = nullptr;
    }
   #else
    if (oscServer != nullptr)
    {
        lo_server_del_method(oscServer, nullptr, nullptr);
        lo_server_free(oscServer);
        oscServer = nullptr;
    }
   #endif
}

void Initializer::stepRemoteServer()
{
    DISTRHO_SAFE_ASSERT_RETURN(oscServer != nullptr,);
    DISTRHO_SAFE_ASSERT_RETURN(remotePluginInstance != nullptr,);

   #ifndef CARDINAL_INIT_OSC_THREAD
    for (;;)
    {
        try {
            if (lo_server_recv_noblock(oscServer, 0) == 0)
                break;
        } DISTRHO_SAFE_EXCEPTION_CONTINUE("stepRemoteServer")
    }
   #endif
}
#endif // HAVE_LIBLO

END_NAMESPACE_DISTRHO
