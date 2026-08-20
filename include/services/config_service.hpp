#pragma once
#include <caf/all.hpp>
#include <string>
#include <map>

// ------------------------------------------------------------------
// 配置服务契约
// 提供方：PlatformPlugin
// 消费方：任何需要读取/修改配置的插件
// ------------------------------------------------------------------
using get_config_atom = caf::atom_constant<caf::atom("getcfg")>;
using set_config_atom = caf::atom_constant<caf::atom("setcfg")>;
