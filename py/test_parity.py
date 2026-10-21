"""Training/inference parity (M0 must-pass test).

Replays recorded games (py/gen_data.py output) in a fresh polyhost process and checks:
1. recording parity and cross-process determinism: at every recorded decision the fresh process
   gives the same observation, legal list and AI choice as the recording;
2. input parity: the evaluation code's network inputs (eval_vs_bot.batch_inputs) equal the
   training code's (train_bc.collate) for the same decisions, array for array;
3. device parity: policy logits on CPU and on MPS agree (same argmax, small difference).

    .venv/bin/python py/test_parity.py data/m1v2 --games 3
"""
import argparse
import glob
import os
import sys

import numpy as np
import torch

import eval_vs_bot as E
import gen_data
import model as M
import polyenv
import train_bc as T


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('data_dir')
    ap.add_argument('--games', type=int, default=3)
    ap.add_argument('--port', type=int, default=8390)
    args = ap.parse_args()
    files = sorted(glob.glob(os.path.join(args.data_dir, 'g*.npz')))[:args.games]
    torch.manual_seed(0)
    net_cpu = M.Net().eval()
    net_mps = M.Net().to('mps').eval() if torch.backends.mps.is_available() else None
    if net_mps is not None:
        net_mps.load_state_dict(net_cpu.state_dict())
    failures, checked, worst = 0, 0, 0.0
    with polyenv.launch(args.port) as env:
        for f in files:
            z = np.load(f)
            seed = int(z['seed'])
            spec = gen_data.game_spec(seed)
            assert tuple(spec.tribes) == tuple(int(t) for t in z['tribes']), 'tribes differ from the recording'
            game = T.load_game(f)
            st = env.new(0, spec)
            k, D = 0, len(z['label'])
            obs_list, st_list = [], []
            while k < D:
                obs = env.observe([(0, 0)])[0]
                label = env.bot([0])[0]
                if label >= 0:
                    o = z['legal_offsets']
                    same = (np.array_equal(obs.tiles.reshape(-1, obs.tiles.shape[-1]), z['tiles'][k]) and
                            np.array_equal(obs.globals, z['globals'][k]) and np.array_equal(obs.techs, z['techs'][k]) and
                            np.array_equal(st.legal, z['legal'][o[k]:o[k + 1]]) and label == int(z['label'][k]) and
                            st.current == int(z['player'][k]))
                    if not same:
                        failures += 1
                        print(f'{os.path.basename(f)} decision {k}: replay differs from the recording')
                        break
                    if k < 256:
                        obs_list.append(obs)
                        st_list.append(st)
                    k += 1
                st = env.step([(0, label if label >= 0 else polyenv.STEP_BOT_OWN)])[0][1]
            checked += k
            # Inputs: evaluation path vs training path, for the first decisions of this game.
            n = len(obs_list)
            a = E.batch_inputs(obs_list, st_list)
            b = T.collate([game], [(0, d) for d in range(n)])
            for key in a:
                if not np.array_equal(a[key], b[key]):
                    failures += 1
                    print(f'{os.path.basename(f)}: input "{key}" differs between evaluation and training')
            # Devices.
            if net_mps is not None:
                lc = E.policy_logits(net_cpu, 'cpu', a)
                lm = E.policy_logits(net_mps, 'mps', a)
                m = a['mask']
                diff = float(np.max(np.abs(lc[m] - lm[m])))
                worst = max(worst, diff)
                if not np.array_equal(lc.argmax(1), lm.argmax(1)):
                    print(f'{os.path.basename(f)}: argmax differs between CPU and MPS on '
                          f'{int((lc.argmax(1) != lm.argmax(1)).sum())} of {n} decisions (max |diff| {diff:.2e})')
                    if diff > 1e-3:
                        failures += 1
            print(f'{os.path.basename(f)}: {k} decisions replayed identically, inputs equal, CPU/MPS max |diff| {worst:.2e}', flush=True)
    print(f'\nparity: {checked} decisions checked, {failures} failures')
    sys.exit(1 if failures else 0)


if __name__ == '__main__':
    main()
