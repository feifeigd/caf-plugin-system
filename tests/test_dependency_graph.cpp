#include "dependency_graph.hpp"
#include <iostream>
#include <cassert>

int main() {
    DependencyGraph g;
    g.register_service("svc_a", "PluginA");
    g.register_service("svc_b", "PluginB");
    g.register_service("svc_c", "PluginC");

    g.add_plugin("PluginA", {});
    g.add_plugin("PluginB", {"svc_a"});
    g.add_plugin("PluginC", {"svc_b"});

    auto order = g.topological_order();
    assert(order.size() == 3);
    assert(order[0] == "PluginA");
    assert(order[1] == "PluginB");
    assert(order[2] == "PluginC");

    DependencyGraph g2;
    g2.register_service("svc_x", "PluginX");
    g2.register_service("svc_y", "PluginY");
    g2.add_plugin("PluginX", {"svc_y"});
    g2.add_plugin("PluginY", {"svc_x"});
    assert(g2.has_cycle_from("PluginX") == true);

    std::cout << "All tests passed." << std::endl;
    return 0;
}
