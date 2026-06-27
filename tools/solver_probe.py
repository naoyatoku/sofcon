"""
Feasibility probe for an EXACT endgame solver (strong teacher).

Game: dig empty cells; whoever takes the LAST cell wins. Move = 1..k connected
cells (here k=3, the day-contest rule). Multiplayer => use "maxn": the player to
move picks any move that lets *them* end up the winner under optimal play
(each player maximizes their own win first).

This probe measures how large `empty_count` can be while still solving in a
reasonable per-call budget, to pick the self-play SOLVE_THRESHOLD.
"""
import sys, os, time
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import numpy as np
from game.board import Board
from game.move_gen import generate_moves


def solve_winner(board: Board, memo: dict) -> int:
    """Return the player_id who wins under optimal (maxn) play from `board`."""
    if board.done:
        return board.winner
    key = (board.grid.tobytes(), board.current_player)
    hit = memo.get(key)
    if hit is not None:
        return hit
    pid = board.current_player
    winner = None
    for move in generate_moves(board):
        nb = board.copy()
        nb.apply_move(move)
        w = solve_winner(nb, memo)
        if w == pid:           # found an immediately-winning line for mover
            winner = pid
            break
        if winner is None:
            winner = w         # fall back to first explored line
    memo[key] = winner
    return winner


def make_endgame(empty: int, H=15, W=15, num_players=2, seed=0) -> Board:
    """A board that is almost full: only `empty` empty cells remain, rest walls."""
    rng = np.random.default_rng(seed)
    grid = np.full((H, W), -1, dtype=np.int8)  # all walls
    # carve a connected-ish blob of empties near top-left
    cells = [(0, 0)]
    occ = {(0, 0)}
    frontier = [(0, 1), (1, 0)]
    while len(cells) < empty and frontier:
        i = rng.integers(len(frontier))
        r, c = frontier.pop(i)
        if (r, c) in occ or not (0 <= r < H and 0 <= c < W):
            continue
        occ.add((r, c)); cells.append((r, c))
        for dr, dc in ((-1,0),(1,0),(0,-1),(0,1)):
            nr, nc = r+dr, c+dc
            if 0 <= nr < H and 0 <= nc < W and (nr, nc) not in occ:
                frontier.append((nr, nc))
    for r, c in cells:
        grid[r, c] = 0
    return Board(grid, num_players)


if __name__ == "__main__":
    for npl in (2, 3):
        print(f"\n=== {npl} players ===")
        for empty in range(6, 22, 2):
            board = make_endgame(empty, num_players=npl, seed=empty)
            memo = {}
            t0 = time.perf_counter()
            try:
                w = solve_winner(board, memo)
                dt = time.perf_counter() - t0
                print(f"empty={empty:2d} | winner={w} | "
                      f"states={len(memo):8d} | {dt*1000:8.1f} ms")
                if dt > 8.0:
                    print("  (exceeds 8s budget — stop scaling here)")
                    break
            except RecursionError:
                print(f"empty={empty:2d} | RecursionError")
                break
