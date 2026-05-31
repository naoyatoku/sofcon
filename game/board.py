"""
Board representation and game logic.

Cell values:
  -1 : wall
   0 : empty (can be dug)
   1+ : claimed by player (player_id = value)
"""

import numpy as np
from typing import List, Tuple, Optional

WALL  = -1
EMPTY =  0

Move = List[Tuple[int, int]]  # list of (row, col), 1-3 cells, orthogonally connected


class Board:
    def __init__(self, grid: np.ndarray, num_players: int):
        """
        grid: 2D np.int8 array, shape (H, W), values: -1=wall, 0=empty
        num_players: 2-6
        """
        self.grid = grid.copy().astype(np.int8)
        self.H, self.W = grid.shape
        self.num_players = num_players
        self.current_player = 1  # 1-indexed
        self.empty_count = int((grid == EMPTY).sum())
        self.done = False
        self.winner: Optional[int] = None

    def copy(self) -> "Board":
        b = Board.__new__(Board)
        b.grid = self.grid.copy()
        b.H, b.W = self.H, self.W
        b.num_players = self.num_players
        b.current_player = self.current_player
        b.empty_count = self.empty_count
        b.done = self.done
        b.winner = self.winner
        return b

    def apply_move(self, move: Move) -> None:
        """Apply move (list of cells) for current player. Advances turn."""
        assert not self.done, "Game is already over"
        pid = self.current_player
        for r, c in move:
            assert self.grid[r, c] == EMPTY, f"Cell ({r},{c}) is not empty"
            self.grid[r, c] = pid
        self.empty_count -= len(move)
        if self.empty_count == 0:
            self.done = True
            self.winner = pid
        else:
            self.current_player = self.current_player % self.num_players + 1

    def is_valid_move(self, move: Move) -> bool:
        if not (1 <= len(move) <= 3):
            return False
        seen = set()
        for r, c in move:
            if not (0 <= r < self.H and 0 <= c < self.W):
                return False
            if self.grid[r, c] != EMPTY:
                return False
            if (r, c) in seen:
                return False
            seen.add((r, c))
        if len(move) == 1:
            return True
        # check orthogonal connectivity
        return _is_connected(move)

    def result(self, player_id: int) -> Optional[float]:
        """Return 1.0 if player won, -1.0 if lost, None if not done."""
        if not self.done:
            return None
        return 1.0 if self.winner == player_id else -1.0


def _is_connected(cells: Move) -> bool:
    """Check if a list of cells forms an orthogonally connected region."""
    cell_set = set(map(tuple, cells))
    visited = set()
    stack = [cells[0]]
    while stack:
        r, c = stack.pop()
        if (r, c) in visited:
            continue
        visited.add((r, c))
        for dr, dc in ((-1,0),(1,0),(0,-1),(0,1)):
            nb = (r+dr, c+dc)
            if nb in cell_set and nb not in visited:
                stack.append(nb)
    return len(visited) == len(cells)
