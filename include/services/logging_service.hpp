#pragma once
#include <caf/all.hpp>
#include <string>

// ------------------------------------------------------------------
// 日志服务契约
// 提供方：LoggerPlugin
// 消费方：任何需要打日志的插件
// ------------------------------------------------------------------
using log_atom = caf::atom_constant<caf::atom("logmsg")>;
