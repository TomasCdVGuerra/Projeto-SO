Change Summary
Date: 2025-11-18

Additional changes (Date: 2025-11-19)

4) Added POSIX-based loader and integrated CLI directory handling
  - Purpose: prepare the program to read `*.lvl`, `*.m`, and `*.p` from a directory using POSIX APIs.
  - Files added: `include/loader.h`, `src/loader.c`
  - Notes: `loader.c` scans the provided directory for `.lvl` files, reads files using `open`/`read`, parses `.lvl` and behavior files (.m/.p) into `board_t`, `pacman_t`, and `ghost_t`. It exposes `load_next_level()` and `reset_level_iterator()` APIs.

5) Integrated loader initialization and cleanup
  - Purpose: ensure the program requires a level directory and prepares the loader.
  - Files modified: `src/game.c`
  - Notes: `game.c` now requires `./Pacmanist <level_directory>`, initializes the loader at startup (`init_level_loader(argv[1])`) and calls `cleanup_level_loader()` on exit.

6) Updated assistant instructions with assignment constraints
  - Purpose: record project-specific constraints for future assistant sessions.
  - Files modified: `.assistant_instructions.md`
  - Notes: Added requirement to update this change-summary file after non-trivial edits; added explicit rules from the project brief (POSIX file APIs only, CLI directory argument, ncurses/thread rules, make/submission expectations).

Notes and next steps
- The local `.change_summary.txt` file was removed and the repository now uses the tracked `CHANGELOG.md` as the canonical change history.
- Next recommended actions:
  - Add more comprehensive example input files under `test_input/` and create a small test runner or CI job to validate parsing and runtime behavior automatically.
  - Add unit/functional tests for the loader and parser where practical.
  - Keep `CHANGELOG.md` updated after non-trivial edits.

---

Additional changes (Date: 2025-11-20)

7) Integrated loader into runtime and test input
  - Purpose: switch runtime to load levels using the new POSIX loader and verify behavior with a sample input.
  - Files modified/added: `src/game.c`, `src/loader.c`, `include/loader.h`, `test_input/1.lvl`, `test_input/pacman.p`
  - Notes:
    - Replaced `load_level(&game_board, ...)` with `load_next_level(&game_board, ...)` in `src/game.c` and added handling for the loader return codes (0 = success, 1 = no more levels, <0 = error).
    - Fixed portability of directory iteration in `src/loader.c` by avoiding `DT_DIR` checks and instead skipping `.`/`..` entries and relying on filename suffixes.
    - Included `board.h` in `include/loader.h` so `board_t` is defined for loader declarations.
    - Added sample input files under `test_input/` (`1.lvl`, `pacman.p`) to exercise the loader. These are minimal examples for manual testing.
    - Built the project and verified `bin/Pacmanist` links successfully.

8) Housekeeping
  - Purpose: consolidate change history and remove redundant local files.
  - Files modified: `.gitignore`, `CHANGELOG.md`
  - Notes:
    - Deleted the local `.change_summary.txt` file (its contents were migrated into `CHANGELOG.md`).
    - Removed the `.change_summary.txt` entry from `.gitignore`.

---

Per-file diffs (concise, human-readable)

`include/loader.h` (new)
- Added declarations:
  - `int init_level_loader(const char *dirpath);`
  - `void cleanup_level_loader(void);`
  - `int loader_is_initialized(void);`
  - `int load_next_level(board_t *board, int accumulated_points);`
  - `void reset_level_iterator(void);`

`src/loader.c` (new)
- New POSIX-based loader implementation. Highlights:
  - Scans provided directory with `opendir`/`readdir` and collects `*.lvl` files.
  - Reads files using `open`/`read` (helper `read_file_to_string`).
  - Parses `.lvl` headers (`DIM`, `TEMPO`, `PAC`, `MON`) and board matrix lines.
  - Parses `.m`/`.p` behavior files (`PASSO`, `POS`, movement commands, `Tn`).
  - Populates `board_t`, `pacman_t`, and `ghost_t` structures directly.
  - Exposes: `init_level_loader`, `load_next_level`, `reset_level_iterator`, `cleanup_level_loader`.

`src/game.c` (modified)
- Changed CLI handling and loader integration:
  - Require `./Pacmanist <level_directory>` and exit with error if missing.
  - Call `init_level_loader(argv[1])` at startup and `cleanup_level_loader()` before exit.

`.assistant_instructions.md` (modified)
- Added requirement to append to `.change_summary.txt` after non-trivial edits.
- Added project-specific constraints (POSIX I/O only, CLI input dir, ncurses/thread rules, `make`/submission expectations).

`.gitignore` (modified)
- Added entries: `obj/`, `bin/`, `.assistant_instructions.md`, `.change_summary.txt`, `.DS_Store`.

---

Additional changes (Date: 2025-11-21)

9) Removed all uses of C stdio (stdio.h / FILE*) and switched to POSIX I/O
  - Purpose: satisfy the project constraint to avoid C stdio APIs and use POSIX file-descriptor I/O exclusively.
  - Files modified: `src/board.c`, `src/loader.c`, `src/game.c`
  - Notes:
    - `src/board.c`: replaced `FILE *` debug logging with a POSIX-based debug logger (`open_debug_file` / `close_debug_file`) that writes via `write()`; implemented a minimal safe formatter supporting `%s`, `%d`, and `%c` for current debug usage. Replaced `sprintf` with `strncpy` where appropriate.
    - `src/loader.c`: removed `snprintf`/`sscanf` usages and replaced them with manual path construction and `strtol`-based numeric parsing (still using `open`/`read` for file contents). Confirmed no `#include <stdio.h>` remains.
    - `src/game.c`: replaced `printf` calls with `write(STDERR_FILENO, ...)` to avoid stdio and added the needed `#include <string.h>` for `strlen`.
    - `src/save.c`: unchanged functionally but already used POSIX `open`/`read`/`write`; verified its usage remains POSIX-only.

10) Verification and build
  - Purpose: ensure changes compile and conform to project flags.
  - Actions: ran `make` with `-std=c17 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Werror` and confirmed final link succeeded producing `bin/Pacmanist`.
  - Notes: resolved minor signed/unsigned comparison warnings and missing `string.h` includes during the conversion.

Next recommended actions
- Commit the changelog update and the recent code changes (if not already committed).
- Optionally extend the debug formatter if you plan to use more complex format specifiers in debug output.
- Add a short entry to the repository README describing how to run the program with a level directory and where the `save.dat` quicksave file is stored.


