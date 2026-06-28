"""
Head-to-head: NN-guided MCTS  vs  pure rollout MCTS.

両者とも同じ MCTS クラスを使い、葉の評価だけが違う:
  - NN側   : ネットの value/policy
  - 純粋側 : ランダムロールアウト + 一様 prior

席を交互に入れ替えて公平に対戦し、NN側の勝率を出す。
"""
import sys, os
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import torch, numpy as np, argparse
from model.network import SofconNet
from model.mcts import MCTS
from train.self_play import make_sample_board


def play(net, nn_sims, roll_sims, device, H, W, NP, nn_seat):
    board = make_sample_board(H=H, W=W, num_players=NP)
    while not board.done:
        cur = board.current_player
        if cur == nn_seat:
            mv = MCTS(net, cur, nn_sims, device, use_rollout=False).run(board)
        else:
            mv = MCTS(None, cur, roll_sims, device, use_rollout=True).run(board)
        board.apply_move(mv)
    return board.winner == nn_seat


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--checkpoint", default="model/checkpoint.pt")
    p.add_argument("--games", type=int, default=20)
    p.add_argument("--nn_sims",   type=int, default=80)
    p.add_argument("--roll_sims", type=int, default=200)
    p.add_argument("--players", type=int, default=2)
    p.add_argument("--device", default="cpu")
    args = p.parse_args()

    ckpt = torch.load(args.checkpoint, map_location=args.device)
    H = ckpt.get("board_h", 9); W = ckpt.get("board_w", 9)
    ch = ckpt.get("channels", 64); nb = ckpt.get("num_blocks", 5)
    NP = args.players
    net = SofconNet(board_h=H, board_w=W, channels=ch, num_blocks=nb)
    net.load_state_dict(ckpt["model"]); net.eval()

    print(f"NN(iter {ckpt.get('iter','?')}, sims={args.nn_sims}) "
          f"vs PureMCTS(sims={args.roll_sims}) | {H}x{W} {NP}P x{args.games}games",
          flush=True)

    wins = 0
    for g in range(args.games):
        nn_seat = (g % NP) + 1
        if play(net, args.nn_sims, args.roll_sims, args.device, H, W, NP, nn_seat):
            wins += 1
        print(f"  game {g+1:3d}/{args.games}: NN winrate {wins/(g+1):.1%}", flush=True)

    print(f"\nNN win rate vs Pure MCTS: {wins/args.games:.1%}")
    base = 1.0 / NP
    if wins/args.games > 0.5:
        print(">> NNが純粋MCTSに勝ち越し。提出をNN版に差し替える価値あり。")
    elif wins/args.games > base:
        print(">> 互角〜やや劣勢。学習継続で逆転を狙う。")
    else:
        print(">> まだ純粋MCTSが上。学習継続。")


if __name__ == "__main__":
    main()
