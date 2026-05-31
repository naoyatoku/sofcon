"""
Verify that C++ inference output matches PyTorch forward pass.
Generates a random board, runs PyTorch, then compiles and runs
a small C++ test binary and compares outputs.

Usage:
    python tools/verify_inference.py --checkpoint model/checkpoint.pt
"""

import subprocess, sys, os, struct, tempfile, argparse
import torch, numpy as np

sys.path.insert(0, os.path.dirname(os.path.dirname(__file__)))
from model.network import SofconNet, board_to_tensor
from game.board import Board

CPP_TEST = r"""
#include <cstdio>
#include "weights.h"
#include "inference.h"
#include "inference_impl.h"

int main() {
    // board_input is written by Python as a binary file
    FILE* f = fopen("_test_input.bin", "rb");
    int C_in, H, W;
    fread(&C_in, 4, 1, f);
    fread(&H,    4, 1, f);
    fread(&W,    4, 1, f);
    int n = C_in * H * W;
    float* inp = new float[n];
    fread(inp, 4, n, f);
    fclose(f);

    float* policy = new float[H*W];
    float  value;
    sofcon_nn::NetWeights wt;
    sofcon_nn::forward(inp, C_in, H, W, wt, policy, value);

    // write outputs
    FILE* g = fopen("_test_output.bin", "wb");
    fwrite(policy, 4, H*W, g);
    fwrite(&value, 4, 1,   g);
    fclose(g);

    delete[] inp;
    delete[] policy;
    return 0;
}
"""

def run(checkpoint: str):
    # load model
    ckpt = torch.load(checkpoint, map_location="cpu")
    state = ckpt["model"] if "model" in ckpt else ckpt
    net = SofconNet(); net.load_state_dict(state); net.eval()

    # random board
    grid = np.zeros((9,9), dtype=np.int8)
    board = Board(grid, 2)
    tensor = board_to_tensor(board)

    with torch.no_grad():
        log_p, v = net(tensor)
    py_policy = torch.exp(log_p).numpy().flatten()
    py_value  = float(v.item())

    # write input binary
    inp = tensor.numpy().astype(np.float32)
    C_in, H, W = inp.shape[1], inp.shape[2], inp.shape[3]
    with open("_test_input.bin", "wb") as f:
        f.write(struct.pack("iii", C_in, H, W))
        f.write(inp.flatten().tobytes())

    # export weights
    os.system(f"python tools/export_weights.py --checkpoint {checkpoint} --output cpp/weights.h")

    # compile
    with open("_test_main.cpp", "w") as f:
        f.write(CPP_TEST)
    ret = subprocess.call(["g++", "-O2", "-std=c++17", "-Icpp",
                           "_test_main.cpp", "-o", "_test_nn"])
    assert ret == 0, "Compilation failed"

    subprocess.call(["./_test_nn"])

    # read output
    with open("_test_output.bin", "rb") as f:
        cpp_policy = np.frombuffer(f.read(4*H*W), dtype=np.float32)
        cpp_value  = struct.unpack("f", f.read(4))[0]

    max_diff_p = float(np.abs(py_policy - cpp_policy).max())
    diff_v     = abs(py_value - cpp_value)
    print(f"Policy max diff: {max_diff_p:.6f}")
    print(f"Value  diff:     {diff_v:.6f}")
    print("PASS" if max_diff_p < 1e-4 and diff_v < 1e-4 else "FAIL — check tolerance")

if __name__ == "__main__":
    p = argparse.ArgumentParser()
    p.add_argument("--checkpoint", default="model/checkpoint.pt")
    run(p.parse_args().checkpoint)
