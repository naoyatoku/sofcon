"""
Legal move generation: all valid 1-3 cell orthogonally-connected subsets
of empty cells for the current board state.
"""

from typing import List, Set, Tuple
from game.board import Board, Move, EMPTY

DIRS = [(-1,0),(1,0),(0,-1),(0,1)]


def generate_moves(board: Board) -> List[Move]:
    """Return all legal moves (deduplicated) for the current player."""
    H, W = board.H, board.W
    grid = board.grid
    seen: Set[Tuple] = set()
    result: List[Move] = []

    for r in range(H):
        for c in range(W):
            if grid[r, c] != EMPTY:
                continue
            _add([(r, c)], seen, result)
            for dr, dc in DIRS:
                r2, c2 = r+dr, c+dc
                if not (0 <= r2 < H and 0 <= c2 < W and grid[r2,c2] == EMPTY):
                    continue
                _add([(r,c),(r2,c2)], seen, result)
                # extend to size-3: any empty neighbor of the 2-cell set
                for rn, cn in _frontier({(r,c),(r2,c2)}, H, W, grid):
                    _add([(r,c),(r2,c2),(rn,cn)], seen, result)

    return result


def _add(move: Move, seen: set, result: list) -> None:
    key = tuple(sorted(move))
    if key not in seen:
        seen.add(key)
        result.append(move)


def _frontier(cells: set, H: int, W: int, grid) -> List[Tuple[int,int]]:
    """Empty orthogonal neighbors of a cell set, not already in the set."""
    nb = set()
    for r, c in cells:
        for dr, dc in DIRS:
            rn, cn = r+dr, c+dc
            if (rn,cn) not in cells and 0<=rn<H and 0<=cn<W and grid[rn,cn]==EMPTY:
                nb.add((rn,cn))
    return list(nb)
