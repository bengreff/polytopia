"""WSP-1 critic audit, the offline analysis (research log WSP-1): calibration by group, outcome
residuals and turn-to-turn drift, from a py/wsp/positions.py table. numpy only.

- **Calibration:** log loss and mean(outcome - estimate) per group (phase, tribe, stars, fog, ...),
  with standard errors clustered by game (positions of one game share its outcome).
- **Outcome residuals:** a logistic regression of the outcome on facts with the critic's logit as an
  offset. A coefficient b on a fact says the critic's logit misses b per unit of it (b < 0: the
  critic values it too highly), given everything else the critic sees.
- **Drift (consistency across a turn):** the estimate at the player's next turn start minus the
  estimate now (turn start to turn start, and turn end to the next turn start, i.e. across the
  opponent's reply), regressed on the facts now. For a calibrated estimate its expectation is 0
  whatever the facts; a fact with a nonzero coefficient is one the critic misjudges now and
  corrects a turn later.

    python py/wsp/audit.py pos_sp.npz [--logit logit_critic] [--kind 0]
"""
import argparse

import numpy as np

UNIT_NAMES = 'None Scout Warrior Rider Knight Defender Ship Battleship Catapult Archer MindBender Swordsman Giant'.split()
TRIBE_NAMES = {2: 'Aimo', 4: 'Bardur', 6: 'Hoodrick', 7: 'Imperius', 8: 'Kickoo', 9: 'Luxidoor', 10: 'Oumaji',
               11: 'Quetzali', 12: 'Vengir', 13: 'Xinxi', 14: 'Yadakk', 15: 'Zebasi'}


def sig(x):
    return 1 / (1 + np.exp(-x))


def logloss(p, y):
    p = np.clip(p, 1e-6, 1 - 1e-6)
    return -(y * np.log(p) + (1 - y) * np.log(1 - p))


def cluster_mean(x, cl):
    """Mean of x and its standard error clustered by cl."""
    m = x.mean()
    u, inv = np.unique(cl, return_inverse=True)
    s = np.bincount(inv, weights=x - m)
    n = len(x)
    return m, np.sqrt((s ** 2).sum()) / n if len(u) > 1 else np.nan


def logit_offset(y, off, X, cl, iters=30, ridge=1e-6):
    """Logistic regression y ~ sigmoid(off + X b) (X includes an intercept column); returns b and
    cluster-robust standard errors."""
    b = np.zeros(X.shape[1])
    for _ in range(iters):
        p = sig(off + X @ b)
        g = X.T @ (y - p)
        H = (X * (p * (1 - p))[:, None]).T @ X + ridge * np.eye(X.shape[1])
        step = np.linalg.solve(H, g)
        b += step
        if np.abs(step).max() < 1e-8:
            break
    p = sig(off + X @ b)
    Hinv = np.linalg.pinv((X * (p * (1 - p))[:, None]).T @ X + ridge * np.eye(X.shape[1]))
    u, inv = np.unique(cl, return_inverse=True)
    S = np.zeros((len(u), X.shape[1]))
    np.add.at(S, inv, X * (y - p)[:, None])
    V = Hinv @ (S.T @ S) @ Hinv
    return b, np.sqrt(np.diag(V))


def ols(y, X, cl):
    b, *_ = np.linalg.lstsq(X, y, rcond=None)
    r = y - X @ b
    XtXi = np.linalg.pinv(X.T @ X)
    u, inv = np.unique(cl, return_inverse=True)
    S = np.zeros((len(u), X.shape[1]))
    np.add.at(S, inv, X * r[:, None])
    V = XtXi @ (S.T @ S) @ XtXi
    return b, np.sqrt(np.diag(V))


def load(path):
    z = dict(np.load(path, allow_pickle=False))
    return z


def feature_matrix(d, names):
    cols = []
    for n in names:
        if n.startswith('own_') and n[4:] in UNIT_NAMES:
            cols.append(d['own_units'][:, UNIT_NAMES.index(n[4:])].astype(float))
        elif n.startswith('enemy_') and n[6:] in UNIT_NAMES:
            cols.append(d['enemy_units'][:, UNIT_NAMES.index(n[6:])].astype(float))
        elif n == 'boats':
            cols.append(d['own_units'][:, [6, 7]].sum(1).astype(float))
        elif n == 'own_total':
            cols.append(d['own_units'].sum(1).astype(float))
        elif n == 'enemy_total':
            cols.append(d['enemy_units'].sum(1).astype(float))
        elif n == 'score_diff':
            cols.append((d['score'].astype(float) - d['opp_score']) / 1000)
        else:
            cols.append(d[n].astype(float))
    return np.stack(cols, 1)


FACTS = ['stars', 'income', 'ntechs', 'cities', 'turn', 'explored', 'own_land', 'enemy_land', 'score_diff',
         'own_Warrior', 'own_Rider', 'own_Knight', 'own_Defender', 'own_Swordsman', 'own_Archer', 'own_Catapult',
         'own_Giant', 'boats', 'enemy_total']


def groups_report(d, p, y, cl, sel, title):
    print(f'\n## {title}')
    print('| group | positions | games | mean est | mean outcome | outcome - est (SE) | log loss |')
    print('|---|---|---|---|---|---|---|')
    ok = y >= 0
    rows = []
    def add(name, m):
        m = m & ok & sel
        if m.sum() < 30:
            return
        r, se = cluster_mean(y[m] - p[m], cl[m])
        rows.append((name, int(m.sum()), len(np.unique(cl[m])), p[m].mean(), y[m].mean(), r, se, logloss(p[m], y[m]).mean()))
    add('all', np.ones(len(y), bool))
    t = d['turn']
    for lo, hi in ((0, 5), (6, 10), (11, 15), (16, 20), (21, 30), (31, 200)):
        add(f'turn {lo}-{hi}', (t >= lo) & (t <= hi))
    for lo, hi in ((0, 2), (3, 5), (6, 9), (10, 14), (15, 24), (25, 999)):
        add(f'stars {lo}-{hi}', (d['stars'] >= lo) & (d['stars'] <= hi))
    for q in range(4):
        e = d['explored']
        a, b = np.quantile(e[sel & ok], [q / 4, (q + 1) / 4])
        add(f'explored q{q + 1} ({a:.2f}-{b:.2f})', (e >= a) & (e <= b))
    add('opponent unseen (opp_known 0)', d['opp_known'] == 0)
    add('opponent known', d['opp_known'] == 1)
    if 'start_turn' in d:
        add('fresh map', d['start_turn'] == 0)
        add('human start', d['start_turn'] > 0)
    if 'opponent' in d:
        add('self-play game', d['opponent'] == -1)
        add('vs pool checkpoint', d['opponent'] >= 100)
    for tr, nm in TRIBE_NAMES.items():
        add(f'tribe {nm}', d['tribe'] == tr)
    for lo, hi in ((0.0, 0.1), (0.1, 0.3), (0.3, 0.5), (0.5, 0.7), (0.7, 0.9), (0.9, 1.0)):
        add(f'est {lo:.1f}-{hi:.1f}', (p >= lo) & (p < hi + 1e-9))
    for r in rows:
        flag = ' **' if abs(r[5]) > 2.5 * r[6] else ''
        print(f'| {r[0]} | {r[1]} | {r[2]} | {r[3]:.3f} | {r[4]:.3f} | {r[5]:+.3f} ({r[6]:.3f}){flag} | {r[7]:.3f} |')


def regression_report(name, b, se, names, scale=1.0, unit=''):
    print(f'\n{name}')
    print('| fact | coefficient (SE) | z |')
    print('|---|---|---|')
    for n, bb, s in zip(names, b, se):
        z = bb / s if s > 0 else 0
        flag = ' **' if abs(z) > 3 else ''
        print(f'| {n} | {bb * scale:+.5f} ({s * scale:.5f}){unit} | {z:+.1f}{flag} |')


def next_turn_pairs(d):
    """Index pairs (i, j): i a position, j the same player's next turn-start position in the same game.
    Returns (start->start pairs, end->next start pairs)."""
    key = d['src'].astype(np.int64) * 10 ** 7 + d['game'].astype(np.int64)
    order = np.lexsort((d['kind'], d['decision'], key))
    ss, es = [], []
    last_start, last_end = {}, {}
    for i in order[::-1]:  # backwards in time: remember each player's next turn start
        k = (key[i], int(d['player'][i]))
        if d['kind'][i] == 0:
            if k in last_start:
                ss.append((i, last_start[k]))
            last_start[k] = i
        else:
            if k in last_start:
                es.append((i, last_start[k]))
    return np.array(ss, np.int64).reshape(-1, 2), np.array(es, np.int64).reshape(-1, 2)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('table')
    ap.add_argument('--logit', default='logit_critic')
    ap.add_argument('--facts', default=','.join(FACTS))
    args = ap.parse_args()
    d = load(args.table)
    lg = d[args.logit].astype(float)
    p = sig(lg)
    y = d['y'].astype(float)
    cl = d['src'].astype(np.int64) * 10 ** 7 + d['game'].astype(np.int64)
    names = args.facts.split(',')
    print(f'# {args.table}, scorer {args.logit}: {len(y)} positions, {len(np.unique(cl))} games, '
          f'{(y >= 0).mean():.1%} with an outcome')
    ok = y >= 0
    for kind, kn in ((0, 'turn start'), (1, 'turn end')):
        m = ok & (d['kind'] == kind)
        print(f'{kn}: log loss {logloss(p[m], y[m]).mean():.4f}, Brier {((p[m] - y[m]) ** 2).mean():.4f}, '
              f'accuracy {((p[m] > 0.5) == (y[m] > 0.5)).mean():.3f}, n {m.sum()}')
        if 'v_old' in d and np.isfinite(d['v_old'][m]).any():
            v = d['v_old'][m].astype(float)
            print(f'  (the policy head, v_old: log loss {logloss(v, y[m]).mean():.4f})')
    groups_report(d, p, y, cl, d['kind'] == 0, 'Calibration at turn starts (the positions the planner scores)')

    X = feature_matrix(d, names)
    m = ok & (d['kind'] == 0)
    Xi = np.concatenate([np.ones((m.sum(), 1)), X[m]], 1)
    b, se = logit_offset(y[m], lg[m], Xi, cl[m])
    regression_report('## Outcome residuals at turn starts: logit(outcome) - critic logit, per unit of each fact',
                      b, se, ['intercept'] + names)
    ss, es = next_turn_pairs(d)
    for pairs, title in ((ss, 'turn start -> next turn start'), (es, 'turn end -> next turn start (across the reply)')):
        if not len(pairs):
            continue
        i, j = pairs[:, 0], pairs[:, 1]
        dp = p[j] - p[i]
        r, s = cluster_mean(dp, cl[i])
        print(f'\n## Drift, {title}: {len(i)} pairs, mean change {r:+.4f} ({s:.4f}), sd {dp.std():.3f}')
        Xd = np.concatenate([np.ones((len(i), 1)), X[i]], 1)
        b, se = ols(dp, Xd, cl[i])
        regression_report('Change in estimate (probability x 1000) per unit of each fact now', b, se, ['intercept'] + names, scale=1000)
        dl = np.clip(lg[j], -8, 8) - np.clip(lg[i], -8, 8)
        b, se = ols(dl, Xd, cl[i])
        regression_report('Change in estimate (logit, clipped to +-8) per unit of each fact now', b, se, ['intercept'] + names)


if __name__ == '__main__':
    main()
