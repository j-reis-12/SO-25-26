# Operating Systems course @ IST

**Project 1 - PacmanIST Game**

Goal: Develop the first version of PacmanIST, a parallel, file‑driven game engine that loads levels, monster
behavior, and optional Pacman behavior from the filesystem using POSIX file descriptors (no stdio). The project
explores parallelism, task synchronization, non‑blocking backups using processes, and POSIX filesystem interfaces,
while extending the base sequential game into a multi‑level system with dynamic behavior defined by external files.

**Project 2 - Multi‑process Game Server**

Goal: Extend PacmanIST into a multi‑process game server capable of handling multiple concurrent games through named
pipes (FIFOs) and signal‑based interaction. The project introduces a full client–server architecture, requiring
robust inter‑process communication, message protocols, asynchronous notifications, and signal handlers for logging
and game state management. This part emphasizes scalable server design and safe concurrent execution.
