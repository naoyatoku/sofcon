"""
Self-play data generation and training loop.
"""

import sys, os
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import torch
import torch.nn.functional as F
import numpy as np
from collections import deque
from typing import List, Tuple
from game.board import Board
from game.move_gen import generate_moves
from model.network import SofconNet, board_to_tensor, MAX_PLAYERS
from model.mcts import MCTS


# (tensor, policy_target[H*W], value_vec[MAX_PLAYERS], value_mask[MAX_PLAYERS])
Sample = Tuple[torch.Tensor, np.ndarray, np.ndarray, np.ndarray]


def make_sample_board(H=9, W=9, num_players=2, wall_ratio=0.1) -> Board:
    """Generate a random board for training."""
    grid = np.zeros((H, W), dtype=np.int8)
    n_walls = int(H * W * wall_ratio)
    idxs = np.random.choice(H * W, n_walls, replace=False)
    for i in idxs:
        grid[i // W, i % W] = -1
    return Board(grid, num_players)


def play_game(net: SofconNet, num_players: int, device: str,
              num_simulations: int = 100,
              board_h: int = 9, board_w: int = 9) -> List[Sample]:
    """
    Play one self-play game, return training samples.
    All players share the same network (self-play).
    """
    board = make_sample_board(H=board_h, W=board_w, num_players=num_players)
    history = []  # (tensor, move_index, policy_dist, player_id)

    while not board.done:
        pid = board.current_player
        mcts = MCTS(net, pid, num_simulations, device)
        moves, visit_dist = mcts.get_policy(board)

        # store anchor-cell policy target (H*W sized)
        policy_target = np.zeros(board.H * board.W, dtype=np.float32)
        for move, prob in zip(moves, visit_dist):
            anchor = min(move)
            policy_target[anchor[0] * board.W + anchor[1]] += prob

        tensor = board_to_tensor(board, device)
        history.append((tensor, policy_target, pid))

        # sample move proportional to visit counts (exploration)
        chosen_idx = np.random.choice(len(moves), p=visit_dist)
        board.apply_move(moves[chosen_idx])

    winner = board.winner
    n = board.num_players
    # value mask: only the n existing relative seats contribute to the loss
    mask = np.zeros(MAX_PLAYERS, dtype=np.float32)
    mask[:n] = 1.0

    samples: List[Sample] = []
    for tensor, policy_target, pid in history:
        value_vec = np.zeros(MAX_PLAYERS, dtype=np.float32)
        value_vec[(winner - pid) % n] = 1.0     # one-hot at winner's relative seat
        samples.append((tensor, policy_target, value_vec, mask))
    return samples


def train(net: SofconNet, optimizer: torch.optim.Optimizer,
          replay_buffer: deque, batch_size: int = 256, device: str = "cpu"):
    if len(replay_buffer) < batch_size:
        return None

    batch = [replay_buffer[i] for i in
             np.random.choice(len(replay_buffer), batch_size, replace=False)]

    tensors   = torch.cat([s[0] for s in batch], dim=0).to(device)
    p_targets = torch.tensor(np.array([s[1] for s in batch]), dtype=torch.float32).to(device)
    v_targets = torch.tensor(np.array([s[2] for s in batch]), dtype=torch.float32).to(device)
    v_masks   = torch.tensor(np.array([s[3] for s in batch]), dtype=torch.float32).to(device)

    log_policy, value = net(tensors)   # value: (B, MAX_PLAYERS) sigmoid win-probs

    loss_p = -(p_targets * log_policy).sum(dim=1).mean()
    # masked binary cross-entropy over valid seats only
    bce = F.binary_cross_entropy(value, v_targets, reduction="none")
    loss_v = (bce * v_masks).sum() / v_masks.sum()
    loss = loss_p + loss_v

    optimizer.zero_grad()
    loss.backward()
    optimizer.step()
    return loss.item()


def run_training(
    iterations: int = 200,
    games_per_iter: int = 4,
    num_simulations: int = 100,
    batch_size: int = 256,
    buffer_size: int = 50000,
    checkpoint_every: int = 5,   # 頻繁に保存
    device: str = "cpu",
    players_min: int = 2,
    players_max: int = MAX_PLAYERS,
    board_h: int = 15,
    board_w: int = 15,
    channels: int = 16,
    num_blocks: int = 3,
    save_path: str = "model/checkpoint.pt",
    resume: bool = True,          # デフォルトで再開
):
    net = SofconNet(board_h=board_h, board_w=board_w,
                    channels=channels, num_blocks=num_blocks).to(device)
    optimizer = torch.optim.Adam(net.parameters(), lr=1e-3, weight_decay=1e-4)
    replay_buffer: deque = deque(maxlen=buffer_size)

    # ---- チェックポイントから再開 ----
    start_iter = 1
    if resume and os.path.exists(save_path):
        ckpt = torch.load(save_path, map_location=device)
        net.load_state_dict(ckpt["model"])
        optimizer.load_state_dict(ckpt["optimizer"])
        start_iter = ckpt["iter"] + 1
        print(f"Resumed from iter {ckpt['iter']} ({save_path})", flush=True)
    else:
        print(f"Starting fresh training (players {players_min}-{players_max}, "
              f"{save_path})", flush=True)

    for it in range(start_iter, iterations + 1):
        net.eval()
        for _ in range(games_per_iter):
            np_players = np.random.randint(players_min, players_max + 1)
            samples = play_game(net, np_players, device, num_simulations,
                                board_h=board_h, board_w=board_w)
            replay_buffer.extend(samples)

        net.train()
        loss = train(net, optimizer, replay_buffer, batch_size, device)

        log = (f"iter {it:4d} | buffer {len(replay_buffer):6d} | loss {loss:.4f}" if loss else
               f"iter {it:4d} | buffer {len(replay_buffer):6d} | (warming up...)")
        print(log, flush=True)

        if it % checkpoint_every == 0:
            torch.save({"model": net.state_dict(),
                        "optimizer": optimizer.state_dict(),
                        "iter": it,
                        "board_h": board_h, "board_w": board_w,
                        "channels": channels, "num_blocks": num_blocks,
                        "players_min": players_min, "players_max": players_max},
                       save_path)
            print(f"  -> checkpoint saved (iter {it})", flush=True)

    print("Training complete.", flush=True)
    return net


if __name__ == "__main__":
    import argparse
    p = argparse.ArgumentParser()
    p.add_argument("--iters",       type=int, default=200)
    p.add_argument("--sims",        type=int, default=100)
    p.add_argument("--games",       type=int, default=4)
    p.add_argument("--players_min", type=int, default=2)
    p.add_argument("--players_max", type=int, default=MAX_PLAYERS)
    p.add_argument("--board_h",     type=int, default=15)
    p.add_argument("--board_w",     type=int, default=15)
    p.add_argument("--channels",    type=int, default=16)
    p.add_argument("--blocks",      type=int, default=3)
    p.add_argument("--device",      default="cpu")
    p.add_argument("--save",        default="model/checkpoint.pt", help="保存先")
    p.add_argument("--no-resume",   action="store_true", help="最初からやり直す")
    args = p.parse_args()

    run_training(iterations=args.iters, games_per_iter=args.games,
                 num_simulations=args.sims,
                 players_min=args.players_min, players_max=args.players_max,
                 board_h=args.board_h, board_w=args.board_w,
                 channels=args.channels, num_blocks=args.blocks,
                 save_path=args.save,
                 device=args.device, resume=not args.no_resume)
