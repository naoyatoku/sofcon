"""
Exact endgame solver — the STRONG TEACHER.

Game: dig empty cells; whoever takes the LAST cell wins. Move = 1..3 connected
cells (day-contest rule). Multiplayer uses "maxn": the player to move prefers any
line where *they* end up the winner; only when no self-win exists does the chosen
line not matter for the value label (they lose either way).

Only call these when `board.empty_count <= SOLVE_THRESHOLD`. Measured cost
(worst-case dense blob, 2p, k=3): empty=12 ~0.56s, empty=14 ~2.1s, empty=16 ~124s
(explodes). A NODE BUDGET caps pathological positions: if a solve exceeds
`NODE_BUDGET` expanded states it is abandoned (returns None / NN fallback), so a
single hard endgame can't stall a self-play game for minutes.
"""
from typing import Dict, Optional, Tuple
import numpy as np
from game.board import Board
from game.move_gen import generate_moves

SOLVE_THRESHOLD = 16     # max empty cells to solve exactly (empty-mask key:
                         # ~2s worst-case dense blob at 16; real games faster)
NODE_BUDGET = 80_000     # max states per solve; ~3-4s worst case, then give up
                         # (empty=16 dense blob uses ~37k, so this leaves margin)


class _BudgetExceeded(Exception):
    """Raised internally when a solve blows past NODE_BUDGET."""


def _solve(board: Board, memo: Dict, budget: list) -> Tuple[int, Optional[list]]:
    """Return (winner_under_optimal_play, best_move_from_here).
    `budget` is a 1-element list used as a mutable countdown counter."""
    if board.done:
        return board.winner, None
    # Canonical key: only WHICH cells are empty + whose turn matter for the
    # winner. The colors of already-filled cells are irrelevant (they never
    # affect legal moves or who digs the last cell), so keying on the empty
    # mask collapses millions of redundant colored states into one.
    key = ((board.grid == 0).tobytes(), board.current_player)
    hit = memo.get(key)
    if hit is not None:
        return hit
    budget[0] -= 1
    if budget[0] <= 0:
        raise _BudgetExceeded
    pid = board.current_player
    best_w, best_move = None, None
    for move in generate_moves(board):
        nb = board.copy()
        nb.apply_move(move)
        w, _ = _solve(nb, memo, budget)
        if best_move is None:
            best_w, best_move = w, move   # fallback line
        if w == pid:                      # mover can force their own win
            best_w, best_move = pid, move
            break
    res = (best_w, best_move)
    memo[key] = res
    return res


def _solve_capped(board: Board, memo: Optional[Dict]):
    """Run _solve under NODE_BUDGET; return (winner, move) or None if abandoned."""
    try:
        return _solve(board, {} if memo is None else memo, [NODE_BUDGET])
    except _BudgetExceeded:
        return None


def solve_value_vec(board: Board, num_players: int,
                    memo: Optional[Dict] = None) -> Optional[np.ndarray]:
    """Exact win-prob vector RELATIVE to board.current_player (one-hot on the
    optimal-play winner), or None if the position is too big/hard to solve."""
    if board.empty_count > SOLVE_THRESHOLD:
        return None
    res = _solve_capped(board, memo)
    if res is None:
        return None
    winner, _ = res
    vec = np.zeros(num_players, dtype=np.float32)
    if winner:
        vec[(winner - board.current_player) % num_players] = 1.0
    return vec


def solve_best_move(board: Board, memo: Optional[Dict] = None):
    """Exact optimal move for the player to move, or None if too big/hard."""
    if board.empty_count > SOLVE_THRESHOLD:
        return None
    res = _solve_capped(board, memo)
    if res is None:
        return None
    return res[1]
