#include "ftec/dag.hpp"

#include <map>
#include <stdexcept>

namespace ftec {

namespace {

// A stable text form of a guard set, used only to decide whether two paths are
// still on the same branch. Constraints arrive in the order the executor
// recorded them, which is the same order for any two paths that agree so far,
// so no sorting is needed.
std::string guard_key(const std::vector<fpdl::SymbolicConstraint>& guards) {
    std::string key;
    for (const auto& guard : guards) {
        key += guard.expected ? "+" : "-";
        key += guard.expression;
        key += '\x1f';
    }
    return key;
}

std::vector<fpdl::SymbolicConstraint> guards_before(const fpdl::SymbolicPath& path,
                                                    std::size_t event_index) {
    // after_event counts the events already executed when the constraint was
    // recorded, so the ones tagged `event_index` are exactly those that must
    // hold before event_index runs (and, at the end, before the terminal).
    std::vector<fpdl::SymbolicConstraint> out;
    for (const auto& constraint : path.constraints) {
        if (constraint.after_event == event_index) out.push_back(constraint);
    }
    return out;
}

CircuitRef circuit_of(const fpdl::SEEvent& event, const std::filesystem::path& source_dir) {
    CircuitRef ref;
    ref.se_name         = event.se_name;
    ref.qasm            = (source_dir / event.qasm_file).lexically_normal();
    ref.data_qubits     = event.data_qubits;
    ref.syndrome_qubits = event.syndrome_qubits;
    ref.flag_qubits     = event.flag_qubits;
    ref.measures        = event.measures;
    return ref;
}

} // namespace

std::vector<std::size_t> Dag::successors(std::size_t node) const {
    std::vector<std::size_t> out;
    for (const auto& edge : edges) {
        if (edge.from == node) out.push_back(edge.to);
    }
    return out;
}

const DagEdge& Dag::edge_to(std::size_t from, std::size_t to) const {
    for (const auto& edge : edges) {
        if (edge.from == from && edge.to == to) return edge;
    }
    throw std::out_of_range("ftec::Dag: no edge between the requested nodes");
}

Dag build_dag(const fpdl::ParseResult& parsed, const std::filesystem::path& source_dir) {
    Dag dag;
    dag.protocol   = parsed.protocol_name;
    dag.code       = parsed.code;
    dag.path_count = parsed.paths.size();
    dag.truncated  = parsed.truncated;

    dag.nodes.push_back(DagNode{DagNode::Kind::Start, 0, 0, 0, {}, {}, 0, {}, {}, {}});

    // Children of each node, keyed by the step that reaches them. Two paths
    // stay merged exactly while these keys agree.
    std::vector<std::map<std::string, std::size_t>> children(1);

    const auto descend = [&](std::size_t parent, const std::string& key,
                             const std::vector<fpdl::SymbolicConstraint>& guards,
                             auto&& make_node) {
        if (const auto found = children[parent].find(key); found != children[parent].end()) {
            return found->second;
        }
        const std::size_t id = dag.nodes.size();
        DagNode node = make_node();
        node.id = id;
        dag.nodes.push_back(std::move(node));
        children.emplace_back();
        dag.edges.push_back(DagEdge{parent, id, guards});
        children[parent].emplace(key, id);
        return id;
    };

    for (const auto& path : parsed.paths) {
        std::size_t current = dag.root();

        for (std::size_t i = 0; i < path.events.size(); ++i) {
            const auto& event  = path.events[i];
            const auto  guards = guards_before(path, i);
            const std::string key = guard_key(guards) + '\x1e' + event.se_name + '#' +
                                    std::to_string(event.round) + '#' +
                                    std::to_string(event.invocation) + '#' + event.phase;

            current = descend(current, key, guards, [&] {
                DagNode node;
                node.kind       = DagNode::Kind::Se;
                node.round      = event.round;
                node.invocation = event.invocation;
                node.phase      = event.phase;
                node.circuit    = circuit_of(event, source_dir);
                return node;
            });
        }

        // Terminals are never merged: each path ends in its own, so that a
        // result can name the path it came from.
        const auto guards = guards_before(path, path.events.size());
        const std::string key =
            guard_key(guards) + '\x1e' + "terminal#" + std::to_string(path.id);

        descend(current, key, guards, [&] {
            DagNode node;
            node.kind               = DagNode::Kind::Terminal;
            node.path_id            = path.id;
            node.terminal_action    = path.terminal_action;
            node.terminal_condition = path.terminal_condition;
            node.decode_record      = path.decode_record;
            return node;
        });
    }

    return dag;
}

} // namespace ftec
