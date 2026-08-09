# Metronome

`//utils/metronome:metronome` publishes an empty `puc::msg::NullMessage` on
`//metronome/1hz` once per second. It borrows the application's existing IPC
directory and worker pool, and owns neither.

The channel retains at most the newest pending tick. Consumers therefore see a
live heartbeat rather than a delayed burst when workers are temporarily busy.
Destroying or stopping the metronome cancels its periodic job without stopping
the shared pool.
