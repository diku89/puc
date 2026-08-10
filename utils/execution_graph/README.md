# Execution graph

`//utils/execution_graph:dependency_graph` stores reusable dependency topology
without requiring a worker pool or executable jobs. It validates cycles,
provides a dense adjacency snapshot, and produces stable dependency-first and
dependent-first layers. Application lifecycle orchestration uses those layers
for startup, rollback, and teardown while retaining its own failure policy.

`//utils/execution_graph:execution_graph` runs reusable directed acyclic graphs
on a `puc::multithreading::JobQueue`. It is independent of the work represented
by a node: frame rendering, indexing, dependency analysis, and other subsystems
can use the same scheduler.

The node key type must satisfy `ExecutionGraphNode`: it is a copyable,
equality-comparable C++ value with `std::hash` support and need not be default
constructible. Each key is associated with any concrete
`multithreading::Job` subtype, checked by the `ExecutionGraphJob` concept.
Different job subtypes may coexist in one graph.

```cpp
puc::multithreading::JobQueue workers(4U);
puc::execution_graph::ExecutionGraph<std::string> graph(workers);

graph.add_node("read", read_job);
graph.add_node("left", left_job);
graph.add_node("right", right_job);
graph.add_node("publish", publish_job);

graph.add_dependency("read", "left");
graph.add_dependency("read", "right");
graph.add_dependency("left", "publish");
graph.add_dependency("right", "publish");

if (graph.start() == puc::execution_graph::Status::OK) {
  const auto result = graph.wait();
}
```

`start()` validates changed topology and submits every root. Whenever a job
finishes, the graph submits each dependent whose complete prerequisite set is
now satisfied. Independent ready branches therefore use the worker pool in
parallel without imposing level-wide barriers. Cycles are rejected before any
node runs.

`wait()` consumes one run result and returns the graph to its idle state. The
same validated topology and jobs can then run again without reconstruction.
Nodes and dependencies may be added only while idle. The graph borrows its
worker pool and never shuts it down or joins it. The owner must keep the pool
active until every graph using it has quiesced and been destroyed.

Jobs retain the ordinary `JobQueue` contract: `execute()` is `noexcept`, and
job-specific results belong to the subsystem that owns the jobs. Execution
graph statuses describe topology and scheduling failures. A job must not call
`wait()` on the graph currently executing it.
