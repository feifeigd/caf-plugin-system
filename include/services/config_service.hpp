#pragma once
#include "common/message_tags.hpp"
#include <caf/all.hpp>
#include <string>
#include <map>

// ------------------------------------------------------------------
// 配置服务契约
// 提供方：PlatformPlugin
// 消费方：任何需要读取/修改配置的插件
// 消息标签（get_config_atom / set_config_atom）见 common/message_tags.def
// ------------------------------------------------------------------
