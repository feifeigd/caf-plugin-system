#pragma once
#include <caf/all.hpp>
#include <string>
#include <map>

// ------------------------------------------------------------------
// 指标服务契约
// 提供方：PlatformPlugin
// 消费方：任何需要上报指标的插件
// ------------------------------------------------------------------
using report_metric_atom = caf::atom_constant<caf::atom("rptmet")>;
using get_metrics_atom   = caf::atom_constant<caf::atom("getmet")>;
