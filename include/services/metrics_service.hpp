#pragma once
#include "common/message_tags.hpp"
#include <caf/all.hpp>
#include <string>
#include <map>

// ------------------------------------------------------------------
// 指标服务契约
// 提供方：PlatformPlugin
// 消费方：任何需要上报指标的插件
// 消息标签（report_metric_atom / get_metrics_atom）见 common/message_tags.def
// ------------------------------------------------------------------
