"""
Monte Carlo Tree Search guided by SofconNet — N-player correct (PUCT).

Value convention (winner / win-probability based, NOT negamax):
  Each node stores the expected win-probability of the player who MOVED INTO it
  (= the parent's player-to-move). Selection then just maximizes a child's Q for
  the parent's player — no sign flipping — which is correct for ANY number of
  players (negamax is only valid for 2-player zero-sum).

Leaf evaluation produces a win-probability vector indexed RELATIVE to the leaf's
current player (k=0 = current player, k=1 = next, ...). During backprop each
ancestor's mover pulls its own win-prob from that single leaf vector via a
relative-seat offset. Sources of the vector:
  - NN mode      : value head (length MAX_PLAYERS), sigmoid win-probs.
  - rollout mode : a uniform-random playout → one-hot on the winner.
  - terminal     : one-hot on board.winner.
"""

import math
import numpy as np
from typing import List, Optional, Tuple
from game.board import Board
from game.move_gen import generate_moves

C_PUCT = 1.5


class MCTSNode:
    __slots__ = ["board", "parent", "move", "children",
                 "visit_count", "value_sum", "prior", "expanded"]

    def __init__(self, board: Board, parent=None, move=None, prior: float = 0.0):
        self.board = board
        self.parent: Optional[MCTSNode] = parent
        self.move = move
        self.children: List[MCTSNode] = []
        self.visit_count: int = 0
        self.value_sum: float = 0.0      # expected win-prob for the player who moved into this node
        self.prior: float = prior
        self.expanded: bool = False

    @property
    def q_value(self) -> float:
        return self.value_sum / self.visit_count if self.visit_count else 0.0

    def child_score(self, child: "MCTSNode") -> float:
        q = child.q_value          # already from THIS node's player perspective
        u = C_PUCT * child.prior * math.sqrt(self.visit_count + 1) / (1 + child.visit_count)
        return q + u


class MCTS:
    def __init__(self, net, my_player_id: int = 1, num_simulations: int = 200,
                 device: str = "cpu", use_rollout: bool = False):
        self.net = net
        self.my_player_id = my_player_id
        self.num_simulations = num_simulations
        self.device = device
        self.use_rollout = use_rollout

    # ------------------------------------------------------------------ #
    #  Public API
    # ------------------------------------------------------------------ #

    def run(self, board: Board) -> List:
        root = self._build_tree(board)
        return max(root.children, key=lambda n: n.visit_count).move

    def get_policy(self, board: Board) -> Tuple[List, np.ndarray]:
        root = self._build_tree(board)
        moves = [c.move for c in root.children]
        visits = np.array([c.visit_count for c in root.children], dtype=np.float32)
        if visits.sum() > 0:
            visits /= visits.sum()
        return moves, visits

    # ------------------------------------------------------------------ #
    #  Core search
    # ------------------------------------------------------------------ #

    def _build_tree(self, board: Board) -> MCTSNode:
        root = MCTSNode(board.copy())
        self._expand(root)
        for _ in range(self.num_simulations):
            self._simulate(root)
        return root

    def _simulate(self, root: MCTSNode) -> None:
        node = self._select(root)
        n = node.board.num_players

        if node.board.done:
            value_vec = self._onehot(node.board.winner, node.board.current_player, n)
        elif self.use_rollout:
            winner = self._rollout(node.board)
            value_vec = self._onehot(winner, node.board.current_player, n)
        elif not node.expanded:
            value_vec = self._expand(node)        # NN value vector (relative to node)
        else:
            value_vec = self._nn_value(node.board)

        self._backprop(node, value_vec)

    def _select(self, node: MCTSNode) -> MCTSNode:
        while node.expanded and node.children and not node.board.done:
            node = max(node.children, key=node.child_score)
        return node

    def _expand(self, node: MCTSNode):
        """Create children with priors; return the node's leaf value vector
        (relative to node's current player). For rollout mode the value vector
        is computed by the caller, so we just create children and return None."""
        if node.board.done:
            node.expanded = True
            return self._onehot(node.board.winner, node.board.current_player,
                                node.board.num_players)
        moves = generate_moves(node.board)
        if not moves:
            node.expanded = True
            return np.zeros(node.board.num_players, dtype=np.float32)

        if self.use_rollout:
            priors = np.ones(len(moves), dtype=np.float32) / len(moves)
            value_vec = None
        else:
            priors, value_vec = self._infer(node.board, moves)

        for move, prior in zip(moves, priors):
            child = node.board.copy()
            child.apply_move(move)
            node.children.append(MCTSNode(child, parent=node, move=move, prior=prior))
        node.expanded = True
        return value_vec

    def _backprop(self, node: Optional[MCTSNode], value_vec: np.ndarray) -> None:
        """value_vec[k] = win-prob of the player k seats after `node`'s current
        player (i.e. relative to the leaf where it was produced)."""
        leaf_cur = node.board.current_player
        n = node.board.num_players
        while node is not None:
            node.visit_count += 1
            if node.parent is not None:
                mover = node.parent.board.current_player
                k = (mover - leaf_cur) % n
                node.value_sum += float(value_vec[k])
            node = node.parent

    # ------------------------------------------------------------------ #
    #  Helpers
    # ------------------------------------------------------------------ #

    @staticmethod
    def _onehot(winner: int, ref_player: int, n: int) -> np.ndarray:
        """Win-prob vector relative to ref_player: 1.0 at the winner's seat."""
        vec = np.zeros(n, dtype=np.float32)
        if winner:
            vec[(winner - ref_player) % n] = 1.0
        return vec

    def _rollout(self, board: Board) -> int:
        sim = board.copy()
        while not sim.done:
            moves = generate_moves(sim)
            if not moves:
                return 0
            sim.apply_move(moves[np.random.randint(len(moves))])
        return sim.winner

    def _nn_value(self, board: Board) -> np.ndarray:
        from model.network import board_to_tensor
        import torch
        tensor = board_to_tensor(board, self.device)
        with torch.no_grad():
            _, value = self.net(tensor)
        return value.cpu().numpy().reshape(-1)[:board.num_players]

    def _infer(self, board: Board, moves: List):
        """One NN call → (move priors, value vector relative to current player)."""
        from model.network import board_to_tensor
        import torch
        tensor = board_to_tensor(board, self.device)
        with torch.no_grad():
            log_policy, value = self.net(tensor)
        policy = torch.exp(log_policy).cpu().numpy().reshape(board.H, board.W)
        value_vec = value.cpu().numpy().reshape(-1)[:board.num_players]

        priors = np.array([policy[min(m)[0], min(m)[1]] for m in moves], dtype=np.float32)
        total = priors.sum()
        if total > 1e-8:
            priors /= total
        else:
            priors = np.ones(len(moves), dtype=np.float32) / len(moves)
        return priors, value_vec
