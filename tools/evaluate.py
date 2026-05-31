"""
Evaluate trained model vs random player. Measures win rate.
"""
import sys, os
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import torch, numpy as np, argparse
from game.board import Board
from game.move_gen import generate_moves
from model.network import SofconNet
from model.mcts import MCTS
from train.self_play import make_sample_board


def random_move(board):
    moves = generate_moves(board)
    return moves[np.random.randint(len(moves))]


def mcts_move(net, board, sims, device, use_rollout=False):
    mcts = MCTS(net, board.current_player, sims, device, use_rollout=use_rollout)
    return mcts.run(board)


def play_match(net, sims, device, board_h, board_w, num_players,
               net_player_id, use_rollout=False):
    """One game: MCTS plays as net_player_id, others random. Return True if MCTS wins."""
    board = make_sample_board(H=board_h, W=board_w, num_players=num_players)
    while not board.done:
        if board.current_player == net_player_id:
            move = mcts_move(net, board, sims, device, use_rollout)
        else:
            move = random_move(board)
        board.apply_move(move)
    return board.winner == net_player_id


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--checkpoint", default="model/checkpoint.pt")
    p.add_argument("--games", type=int, default=40)
    p.add_argument("--sims",  type=int, default=50)
    p.add_argument("--device", default="cpu")
    p.add_argument("--rollout", action="store_true",
                   help="ネットを使わず純粋MCTS(ランダムロールアウト)で検索の正しさを検証")
    p.add_argument("--board_h", type=int, default=9)
    p.add_argument("--board_w", type=int, default=9)
    p.add_argument("--players", type=int, default=2)
    args = p.parse_args()

    if args.rollout:
        net = None
        board_h, board_w, num_players = args.board_h, args.board_w, args.players
        print(f"[PURE MCTS rollout] {board_h}x{board_w}, {num_players}P, "
              f"{args.games} games, sims={args.sims}")
    else:
        ckpt = torch.load(args.checkpoint, map_location=args.device)
        board_h = ckpt.get("board_h", 9)
        board_w = ckpt.get("board_w", 9)
        num_players = args.players      # net is player-agnostic; choose eval count
        net = SofconNet(board_h=board_h, board_w=board_w)
        net.load_state_dict(ckpt["model"])
        net.eval()
        print(f"[NET iter {ckpt.get('iter','?')}] {board_h}x{board_w}, "
              f"{num_players}P, {args.games} games, sims={args.sims}")

    wins = 0
    for g in range(args.games):
        net_id = (g % num_players) + 1  # rotate seat for fairness
        if play_match(net, args.sims, args.device, board_h, board_w,
                      num_players, net_id, use_rollout=args.rollout):
            wins += 1
        print(f"  game {g+1:3d}/{args.games}: net winrate so far {wins/(g+1):.1%}",
              flush=True)

    expected_random = 1.0 / num_players
    print(f"\nNet win rate: {wins/args.games:.1%}")
    print(f"Random baseline (if equal): {expected_random:.1%}")
    if wins/args.games > expected_random + 0.15:
        print(">> ランダムより明確に強い [OK]")
    elif wins/args.games > expected_random:
        print(">> ランダムよりやや強い（学習はできているが伸びしろ大）")
    else:
        print(">> まだランダム以下。要改善。")


if __name__ == "__main__":
    main()
