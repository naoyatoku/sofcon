"""
Neural network: shared CNN trunk + policy head + value head.

Player-count-AGNOSTIC design (works for any num_players in 1..MAX_PLAYERS).

Input tensor shape: (batch, C_in, H, W),  C_in = 2 + MAX_PLAYERS + 1 = 9
  Channel layout (perspective-relative — always from the current player's view):
    0                : wall mask   (1 where wall)
    1                : empty mask   (1 where empty)
    2 + k (k=0..5)   : occupancy of the player who moves k turns from NOW.
                       k=0 is the current player, k=1 the next, etc.
                       Cells of players that don't exist stay 0.
    last (8)         : num_players normalized = (num_players-1)/(MAX_PLAYERS-1)

  Because the encoding is relative to the current player, ONE network handles
  every seat and every player count without retraining the architecture.

Policy output: log-probabilities over all cells (H*W). Anchor-cell policy:
  the move's top-left cell is the anchor; MCTS handles multi-cell shape.

Value output: a VECTOR of length MAX_PLAYERS, value[k] = win probability of the
  player who moves k turns from now (k=0 = current player). N-player correct.
  For an N-player game only entries k=0..N-1 are meaningful.
"""

import torch
import torch.nn as nn
import torch.nn.functional as F

MAX_PLAYERS = 6
C_IN = 2 + MAX_PLAYERS + 1   # = 9


class ResBlock(nn.Module):
    def __init__(self, channels: int):
        super().__init__()
        self.conv1 = nn.Conv2d(channels, channels, 3, padding=1, bias=False)
        self.bn1   = nn.BatchNorm2d(channels)
        self.conv2 = nn.Conv2d(channels, channels, 3, padding=1, bias=False)
        self.bn2   = nn.BatchNorm2d(channels)

    def forward(self, x):
        residual = x
        x = F.relu(self.bn1(self.conv1(x)))
        x = self.bn2(self.conv2(x))
        return F.relu(x + residual)


class SofconNet(nn.Module):
    def __init__(self, board_h: int = 15, board_w: int = 15,
                 channels: int = 64, num_blocks: int = 5):
        super().__init__()
        self.board_h = board_h
        self.board_w = board_w

        self.stem = nn.Sequential(
            nn.Conv2d(C_IN, channels, 3, padding=1, bias=False),
            nn.BatchNorm2d(channels),
            nn.ReLU(),
        )
        self.blocks = nn.Sequential(*[ResBlock(channels) for _ in range(num_blocks)])

        # Policy head (anchor cell over the board)
        self.policy_conv = nn.Conv2d(channels, 2, 1, bias=False)
        self.policy_bn   = nn.BatchNorm2d(2)
        self.policy_fc   = nn.Linear(2 * board_h * board_w, board_h * board_w)

        # Value head -> per-relative-player win-prob vector (length MAX_PLAYERS)
        self.value_conv = nn.Conv2d(channels, 1, 1, bias=False)
        self.value_bn   = nn.BatchNorm2d(1)
        self.value_fc1  = nn.Linear(board_h * board_w, 256)
        self.value_fc2  = nn.Linear(256, MAX_PLAYERS)

    def forward(self, x: torch.Tensor):
        """
        x: (B, C_in, H, W)
        returns:
          log_policy (B, H*W)
          value      (B, MAX_PLAYERS)  -- win prob per relative player (k=0 current)
        """
        x = self.stem(x)
        x = self.blocks(x)

        p = F.relu(self.policy_bn(self.policy_conv(x)))
        p = p.flatten(1)
        log_policy = F.log_softmax(self.policy_fc(p), dim=1)

        v = F.relu(self.value_bn(self.value_conv(x)))
        v = v.flatten(1)
        v = F.relu(self.value_fc1(v))
        value = torch.sigmoid(self.value_fc2(v))   # win prob in (0,1) per player

        return log_policy, value


def board_to_tensor(board, device="cpu") -> torch.Tensor:
    """Convert a Board to a (1, C_IN, H, W) perspective-relative float tensor."""
    import numpy as np
    from game.board import WALL, EMPTY

    H, W = board.H, board.W
    n = board.num_players
    cur = board.current_player           # 1-indexed
    arr = np.zeros((C_IN, H, W), dtype=np.float32)

    arr[0] = (board.grid == WALL).astype(np.float32)
    arr[1] = (board.grid == EMPTY).astype(np.float32)
    # perspective-relative occupancy: k = 0 is current player
    for k in range(n):
        pid = (cur - 1 + k) % n + 1      # player who moves k turns from now
        arr[2 + k] = (board.grid == pid).astype(np.float32)
    arr[C_IN - 1] = (n - 1) / (MAX_PLAYERS - 1)

    return torch.from_numpy(arr).unsqueeze(0).to(device)
