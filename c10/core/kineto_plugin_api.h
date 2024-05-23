#pragma once

#ifdef USE_KINETO

#include <c10/macros/Macros.h>
#include <c10/core/DeviceType.h>

#include <libkineto.h>

namespace c10 {
namespace kineto_plugin {

using PluginFactory = libkineto::ChildActivityProfilerFactory;
void C10_API insertKinetoPluginRegister(DeviceType t, PluginFactory f);
void C10_API removeKinetoPluginRegister(DeviceType t);
void C10_API registerKinetoPluginProfiler();
void C10_API registerKinetoPluginProfiler(DeviceType t);

struct KinetoPluginRegister {
  explicit KinetoPluginRegister(DeviceType t, PluginFactory f) {
    c10::kineto_plugin::insertKinetoPluginRegister(t, f);
  }
};

#define REGISTER_KINETO_PLUGIN(t, f)                            \
  namespace {                                                   \
  static KinetoPluginRegister kineto_plugin_register(t, f);     \
  }

struct KinetoPluginRemover {
  explicit KinetoPluginRemover(DeviceType t) {
    c10::kineto_plugin::removeKinetoPluginRegister(t);
  }
};

#define UNREGISTER_KINETO_PLUGIN(t)                             \
  namespace {                                                   \
  static KinetoPluginRemover kineto_plugin_remover(t);          \
  }

}
}

#endif
