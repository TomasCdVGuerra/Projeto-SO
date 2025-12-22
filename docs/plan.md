# Plan for Project Part 2

## Recommendation
We recommend using the **teacher's solution (`SO-2526-sol-parte1`)** as the base for Part 2.
**Reasons:**
1.  **Stability:** It provides a verified, correct implementation of Part 1.
2.  **Simplicity:** The synchronization model (RW locks) is simpler than the fine-grained locking in the current implementation, making it easier to adapt for the server-client architecture.
3.  **Focus:** Using the provided solution allows us to focus entirely on the new requirements (processes, pipes, IPC) rather than debugging or maintaining the previous part's logic.

## Differences Analysis
| Feature | Current Implementation (`src/`) | Teacher's Solution (`SO-2526-sol-parte1/src/`) |
| :--- | :--- | :--- |
| **Synchronization** | Fine-grained mutexes per cell, Barriers. | Coarse-grained `pthread_rwlock` for board state. |
| **Level Loading** | Dedicated `loader.c` with sorting. | `readdir` loop in `game.c` (no explicit sorting). |
| **Threading** | `init_threads` function, barrier synchronization. | Threads created/joined inside the main level loop. |
| **Structure** | `loader.c` for file handling. | `parser.c` for file parsing. |

## Implementation Plan

### Phase 1: Setup & Migration
1.  Copy `SO-2526-sol-parte1/src/*` to a new working directory (or overwrite `src/` after backing up).
2.  Verify compilation and execution of the base code.

### Phase 2: Server Implementation (Exercise 1.1 & 1.2)
The `PacmanIST` executable will become the server.

1.  **Arguments:** Update `main` to accept `levels_dir`, `max_games`, and `fifo_name`.
2.  **Initialization:**
    *   Create the server's named pipe (FIFO) using `mkfifo`.
    *   Initialize synchronization primitives for the session buffer.
3.  **Host Thread (Tarefa Anfitriã):**
    *   Create a thread that opens the server FIFO for reading.
    *   Loop to receive connection requests (`pacman_connect` protocol).
    *   Parse request: `req_pipe_path`, `notif_pipe_path`.
    *   If active sessions < `max_games`, accept connection.
    *   Put client info into a producer-consumer buffer.
4.  **Game Threads (Tarefas Gestoras):**
    *   Create `max_games` threads at startup.
    *   Each thread waits for a client from the buffer.
    *   **Session Loop:**
        *   Send confirmation to client via `notif_pipe`.
        *   **Game Loop:**
            *   Receive commands from `req_pipe` (non-blocking or separate thread?). *Correction: The server needs to read commands. The architecture suggests a thread per client.*
            *   Periodically send board state to `notif_pipe`.
            *   Handle `pacman_disconnect`.
5.  **Game Logic Adaptation:**
    *   Disable local `ncurses` drawing on the server.
    *   The server runs the game logic (monsters, physics).
    *   Instead of `draw_board` to screen, serialize the board state and write to the client's notification pipe.

### Phase 3: Client Implementation
The `client` executable.

1.  **Arguments:** `id`, `server_fifo`, `input_file` (optional).
2.  **API Implementation (`api.c`):**
    *   `pacman_connect`: Create client FIFOs, send connect request to server, wait for ack.
    *   `pacman_disconnect`: Send disconnect opcode, close/unlink FIFOs.
    *   `pacman_play`: Send move command opcode + direction to request FIFO.
    *   `receive_board_updates`: Read opcode + data from notification FIFO, deserialize, update local board, draw with ncurses.
3.  **Client Main Loop:**
    *   Call `pacman_connect`.
    *   **Input Thread:** Read keys from stdin (or file) -> call `pacman_play`.
    *   **Update Thread (Main):** Loop calling `receive_board_updates`.

### Phase 4: Signal Handling (Exercise 2)
1.  **Server:**
    *   Block `SIGUSR1` in all threads except the Host Thread.
    *   Install `SIGUSR1` handler in Host Thread (or use `sigwait`).
    *   **Handler/Action:**
        *   Iterate over active games.
        *   Collect scores.
        *   Sort and write top 5 to a log file.

## Next Steps
1.  Confirm if we should proceed with overwriting `src/` with the teacher's solution.
2.  Start with **Phase 1**.
