"""
Bundle prog.cpp + NN headers into a single self-contained source file.

Usage:
    python tools/bundle.py \
        --checkpoint model/checkpoint_16ch_15x15.pt \
        --output prog_submit.cpp

What it does:
    1. Export weights to a temporary weights.h
    2. Replace the '#if __has_include("cpp/weights.h")' block in prog.cpp
       with the inlined content of inference.h + weights.h + inference_impl.h
    3. Write the result to prog_submit.cpp (single file, no extra headers needed)
"""

import argparse
import os
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


NN_BLOCK_START = '#if __has_include("cpp/weights.h")'
NN_BLOCK_END   = '#endif'


def find_nn_block(lines):
    """Return (start_idx, end_idx) of the __has_include block (inclusive)."""
    start = None
    for i, line in enumerate(lines):
        if NN_BLOCK_START in line:
            start = i
        if start is not None and line.strip() == NN_BLOCK_END:
            return start, i
    raise ValueError(f"Could not find '{NN_BLOCK_START}' ... '{NN_BLOCK_END}' in prog.cpp")


def read_file(path, strip_local_includes=False):
    with open(path, encoding="utf-8") as f:
        lines = f.readlines()
    if strip_local_includes:
        cleaned = []
        for line in lines:
            s = line.strip()
            # #pragma once と ローカル #include "*.h" を除去
            if s == "#pragma once":
                continue
            if s.startswith('#include "') and s.endswith('"'):
                continue
            cleaned.append(line)
        return "".join(cleaned)
    return "".join(lines)


def bundle(checkpoint_path, output_path):
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

    # 1. Export weights to a temp file
    with tempfile.NamedTemporaryFile(suffix=".h", delete=False, mode="w",
                                     encoding="utf-8") as tmp:
        tmp_weights = tmp.name

    try:
        from tools.export_weights import export
        export(checkpoint_path, tmp_weights)

        # 2. Read all parts
        prog_lines = read_file(os.path.join(root, "prog.cpp")).splitlines(keepends=True)
        inference_h    = read_file(os.path.join(root, "cpp", "inference.h"),    strip_local_includes=True)
        weights_h      = read_file(tmp_weights,                                  strip_local_includes=True)
        inference_impl = read_file(os.path.join(root, "cpp", "inference_impl.h"), strip_local_includes=True)

        # 3. Locate the __has_include block
        start, end = find_nn_block(prog_lines)

        # 4. Build the replacement block
        replacement = (
            "// ---- NN inference (inlined by tools/bundle.py) ----\n"
            + inference_h + "\n"
            + weights_h   + "\n"
            + inference_impl + "\n"
            + "#define USE_NN 1\n"
            + "// ---- end NN inference ----\n"
        )

        # 5. Splice
        out_lines = prog_lines[:start] + [replacement] + prog_lines[end+1:]

        with open(output_path, "w", encoding="utf-8") as f:
            f.writelines(out_lines)

        print(f"Bundled -> {output_path}  ({os.path.getsize(output_path)//1024} KB)")

    finally:
        os.unlink(tmp_weights)


if __name__ == "__main__":
    p = argparse.ArgumentParser()
    p.add_argument("--checkpoint", default="model/checkpoint_16ch_15x15.pt")
    p.add_argument("--output",     default="prog_submit.cpp")
    args = p.parse_args()
    bundle(args.checkpoint, args.output)
