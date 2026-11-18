"""Puts the htdemucs_6s model files into a folder that the analyzer uses offline:

    python tools/fetch_demucs_model.py <models-dir>

Downloads the model once (into the torch hub cache) and copies htdemucs_6s.yaml and its .th
file. The standalone build ships this folder next to b4nd-analyzer(.exe)."""

import glob
import os
import shutil
import sys

import demucs
import torch
import yaml
from demucs.pretrained import get_model

NAME = "htdemucs_6s"


def main(out):
    get_model(NAME)   # downloads into the torch hub cache when missing
    os.makedirs(out, exist_ok=True)
    spec = os.path.join(os.path.dirname(demucs.__file__), "remote", NAME + ".yaml")
    shutil.copy(spec, os.path.join(out, NAME + ".yaml"))
    with open(spec) as f:
        sigs = yaml.safe_load(f)["models"]
    hub = os.path.join(torch.hub.get_dir(), "checkpoints")
    for sig in sigs:
        files = glob.glob(os.path.join(hub, sig + "-*.th"))
        if not files:
            sys.exit(f"model file {sig}-*.th not in {hub}")
        shutil.copy(files[0], out)
        print("copied", os.path.basename(files[0]), os.path.getsize(files[0]) // 1024, "KB")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1]))
