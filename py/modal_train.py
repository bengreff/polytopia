"""Runs py/train_bc.py on a cloud GPU with Modal (free Starter credits), for when the PC is away.

Data and runs live in the private Modal volume "polydata": data under /data/<name>, runs under
/runs/<name>. Only our generated game records and our code go up, nothing of Midjiwan's.

    .venv/bin/modal volume create polydata                      # once
    # upload games as a few tar archives (a long many-file upload can drop), then unpack them:
    .venv/bin/modal volume put polydata part_aa.tar /upload/<name>/part_aa.tar
    .venv/bin/modal run py/modal_train.py::unpack --name <name>
    .venv/bin/modal run py/modal_train.py::main --run r1_smoke --data m1v2 --args "--min_seed 18000 --minutes 60"
    .venv/bin/modal volume get polydata /runs/r1_smoke/last.pt .
"""
import os
import shlex
import subprocess

import modal

app = modal.App('polytopia-train')
volume = modal.Volume.from_name('polydata', create_if_missing=True)
here = os.path.dirname(os.path.abspath(__file__))
image = (modal.Image.debian_slim(python_version='3.12')
         .apt_install('gcc')  # for torch.compile
         .pip_install('torch==2.14.1', 'numpy')
         .add_local_file(os.path.join(here, 'model.py'), '/code/model.py')
         .add_local_file(os.path.join(here, 'train_bc.py'), '/code/train_bc.py')
         .add_local_file(os.path.join(here, 'polyenv.py'), '/code/polyenv.py'))


@app.function(image=image, gpu='L4', volumes={'/vol': volume}, timeout=6 * 3600, cpu=8, memory=32768)
def train(run: str, data: str, args: str):
    cmd = ['python', '/code/train_bc.py', f'/vol/data/{data}', f'/vol/runs/{run}', '--workers', '6'] + shlex.split(args)
    print(' '.join(cmd), flush=True)
    env = dict(os.environ, CC='gcc')
    proc = subprocess.Popen(cmd, cwd='/code', env=env)
    try:
        while proc.poll() is None:
            try:
                proc.wait(timeout=300)
            except subprocess.TimeoutExpired:
                volume.commit()  # make checkpoints and logs visible while it runs
    finally:
        volume.commit()
    if proc.returncode:
        raise RuntimeError(f'train_bc.py exited with {proc.returncode}')


@app.function(image=image, volumes={'/vol': volume}, timeout=3600)
def unpack_remote(name: str):
    import glob
    import tarfile
    os.makedirs(f'/vol/data/{name}', exist_ok=True)
    for t in sorted(glob.glob(f'/vol/upload/{name}/*.tar')):
        with tarfile.open(t) as tar:
            tar.extractall(f'/vol/data/{name}')
        print('unpacked', t, flush=True)
    volume.commit()
    n = len([f for f in os.listdir(f'/vol/data/{name}') if f.endswith('.npz')])
    print(f'/data/{name}: {n} games', flush=True)
    return n


@app.local_entrypoint()
def unpack(name: str):
    unpack_remote.remote(name)


@app.local_entrypoint()
def main(run: str, data: str, args: str = ''):
    train.remote(run, data, args)
