# UDP producer-consumer experiment

This version keeps the file pipeline and `telemetry_core` interfaces unchanged. The UDP receiver reuses `BoundedQueue` and `Statistics`, but splits its work across three roles:

```text
main thread       owns the stop signal and joins both workers
producer thread   recvfrom -> parse seq,value -> queue.push -> queue.close
consumer thread   queue.pop -> values, until the closed queue is empty
```

## Build on Windows

Run these commands in a Visual Studio Developer PowerShell:

```powershell
cmake -S . -B build
cmake --build build --config Debug
```

## Run

Start the receiver in one terminal:

```powershell
.\build\Debug\telemetry_udp_receiver.exe
```

Start the sender in another terminal:

```powershell
.\build\Debug\telemetry_udp_sender.exe
```

The receiver now runs continuously. Press `Ctrl+C` in the receiver terminal to request a graceful stop.

For a non-interactive demonstration or test, pass a duration in milliseconds:

```powershell
.\build\Debug\telemetry_udp_receiver.exe 5000
```

## Graceful-stop sequence

1. `Ctrl+C` or the optional timer sets `stop_requested`.
2. `recvfrom` has a 200 ms timeout, so the producer cannot remain blocked forever when no packets arrive.
3. The producer leaves its receive loop and calls `queue.close()`.
4. The consumer continues to pop all values already in the queue. It exits only after the queue is both closed and empty.
5. The main thread joins the producer and consumer before calculating final statistics.
6. A worker exception is stored in `std::exception_ptr`; after both joins, the main thread rethrows and reports it.
