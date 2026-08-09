# Multithreading

`//utils/multithreading:multithreading` provides PUC's fixed worker pool and
job scheduler. It supports immediate FIFO work, delayed work, and fixed-delay
periodic work without allowing one periodic job to overlap itself.

Jobs are asynchronous and have no result path. A job must handle recoverable
environmental conditions by converging on a later invocation; an escaping
failure terminates the process through its `noexcept` execution boundary.
Submission returns only whether the queue accepted the job.

```cpp
class Refresh final : public puc::multithreading::Job {
 public:
  void execute() noexcept override { refresh_terminal_state(); }
};

puc::multithreading::JobQueue workers(2);
workers.add_urgent(std::make_shared<Refresh>());
puc::multithreading::PeriodicJobHandle refresh;
workers.add_periodic(1000, std::make_shared<Refresh>(), refresh);

// Stops only this periodic job; the shared worker pool remains available.
refresh.cancel();
```

`shutdown()` rejects new work and discards queued work while allowing jobs that
already began to finish. `wait()` performs that shutdown and joins all workers;
it is therefore the usual teardown operation.
