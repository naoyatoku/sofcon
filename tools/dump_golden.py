"""
Golden-reference dumper for a HAND-WRITTEN C++ forward pass.

Loads the trained net, runs ONE fixed deterministic input, and dumps the
intermediate activations after every major stage:

    input  -> stem -> block0 -> block1 -> block2 -> policy(probs) / value(vec)

Each stage is written as raw float32 (C-order) to golden/<name>.bin, with a
manifest golden/manifest.txt listing name/shape, plus golden/summary.txt with
a norm + first 8 values so you can eyeball matches without writing a binary
reader first.

Your C++ forward can dump the same stages and diff against these. The first
stage that disagrees is where your bug is.

Usage:
    python tools/dump_golden.py --checkpoint model/checkpoint_16ch_15x15.pt
"""
import os, sys, argparse, struct
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import numpy as np
import torch
from model.network import SofconNet, board_to_tensor, MAX_PLAYERS
from game.board import Board

OUT_DIR = "golden"


def _write_bin(name, arr):
    arr = np.ascontiguousarray(arr, dtype=np.float32)
    path = os.path.join(OUT_DIR, name + ".bin")
    with open(path, "wb") as f:
        f.write(struct.pack("i", arr.ndim))
        for s in arr.shape:
            f.write(struct.pack("i", s))
        f.write(arr.tobytes())
    return arr


def run(checkpoint, seed):
    os.makedirs(OUT_DIR, exist_ok=True)
    ckpt = torch.load(checkpoint, map_location="cpu")
    H = ckpt.get("board_h", 15); W = ckpt.get("board_w", 15)
    ch = ckpt.get("channels", 16); nb = ckpt.get("num_blocks", 3)
    net = SofconNet(board_h=H, board_w=W, channels=ch, num_blocks=nb)
    net.load_state_dict(ckpt["model"]); net.eval()

    # Fixed deterministic board: a seeded random 2-player position.
    rng = np.random.default_rng(seed)
    grid = np.zeros((H, W), dtype=np.int8)
    # a few walls + a few claimed cells so all input channels are exercised
    for _ in range(H * W // 8):
        grid[rng.integers(H), rng.integers(W)] = -1
    for _ in range(H * W // 8):
        r, c = rng.integers(H), rng.integers(W)
        if grid[r, c] == 0:
            grid[r, c] = rng.integers(1, 3)   # player 1 or 2
    board = Board(grid, 2)
    board.current_player = 1
    x = board_to_tensor(board)                # (1, C_in, H, W)

    stages = {}
    stages["input"] = x

    # capture stage outputs with forward hooks
    captured = {}
    def hook(name):
        def _h(_m, _in, out): captured[name] = out.detach()
        return _h
    net.stem.register_forward_hook(hook("stem"))
    for i, blk in enumerate(net.blocks):
        blk.register_forward_hook(hook(f"block{i}"))

    with torch.no_grad():
        log_policy, value = net(x)

    stages["stem"] = captured["stem"]
    for i in range(nb):
        stages[f"block{i}"] = captured[f"block{i}"]
    stages["policy_probs"] = torch.exp(log_policy)      # (1, H*W)
    stages["value_vec"] = value                          # (1, MAX_PLAYERS)
    stages["value0_sigmoid"] = value[:, :1]              # current player's win prob

    manifest = open(os.path.join(OUT_DIR, "manifest.txt"), "w")
    summary  = open(os.path.join(OUT_DIR, "summary.txt"),  "w")
    summary.write(f"checkpoint iter={ckpt.get('iter','?')}  "
                  f"H={H} W={W} channels={ch} blocks={nb}  seed={seed}\n\n")
    order = ["input", "stem"] + [f"block{i}" for i in range(nb)] + \
            ["policy_probs", "value_vec", "value0_sigmoid"]
    for name in order:
        arr = _write_bin(name, stages[name].numpy())
        flat = arr.flatten()
        manifest.write(f"{name}\t{list(arr.shape)}\n")
        head = " ".join(f"{v:+.6f}" for v in flat[:8])
        summary.write(f"{name:16s} shape={str(list(arr.shape)):20s} "
                      f"L2={np.linalg.norm(flat):.6f}\n   first8: {head}\n")
    manifest.close(); summary.close()

    print(f"Wrote {len(order)} golden stages to {OUT_DIR}/  (iter "
          f"{ckpt.get('iter','?')})")
    print(open(os.path.join(OUT_DIR, "summary.txt")).read())


if __name__ == "__main__":
    p = argparse.ArgumentParser()
    p.add_argument("--checkpoint", default="model/checkpoint_16ch_15x15.pt")
    p.add_argument("--seed", type=int, default=42)
    a = p.parse_args()
    run(a.checkpoint, a.seed)
