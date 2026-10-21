"""The M1 network (docs/design/architecture.md, Layer 1): a transformer over the 196 tile tokens
plus one global token, with
- a pointer policy over the game's legal-command list (one score per row, softmax over the rows),
- a value head (probability that the player to move wins),
- an ownership head (each tile's final owner, relative to the player to move).

Inputs are polyenv observations (obs version 1) and legal rows (type, a, b, arg). Nothing here
judges a position; it only encodes facts.
"""
import torch
import torch.nn as nn
import torch.nn.functional as F

import polyenv

T = {n: i for i, n in enumerate(polyenv.TILE_FEATURES)}
G = {n: i for i, n in enumerate(polyenv.GLOBAL_FEATURES)}

# Categorical tile features and their vocabulary sizes (values are clamped into range).
TILE_CATEGORICAL = {
    'explored': 2, 'terrain': 16, 'climate': 18, 'owner': 4, 'capital_of': 4, 'resource': 16, 'road': 2,
    'route': 2, 'improvement': 64, 'imp_owner': 4, 'imp_level': 16, 'imp_border': 8, 'imp_connected': 4,
    'unit': 64, 'unit_owner': 4, 'unit_promoted': 4, 'unit_moved': 2, 'unit_attacked': 2, 'unit_passenger': 64,
}
TILE_BITS = ['effects', 'imp_rewards', 'imp_effects', 'unit_effects']  # small enum lists as 16-bit sets
TILE_SCALED = {'imp_population': 10, 'imp_production': 5, 'imp_xp': 10, 'unit_health': 100,
               'unit_max_health': 100, 'unit_xp': 10, 'unit_age': 20}
TILE_POINTERS = ['ruling_city', 'unit_home']  # tile indices; encoded as offset and "is this tile"

GLOBAL_CATEGORICAL = {'seat': 2, 'tribe': 18, 'opp_tribe': 18, 'opp_known': 2, 'relation': 4, 'my_embassy': 4,
                      'their_embassy': 4, 'trigger': 4}
GLOBAL_SCALED = {'turn': 50, 'stars': 20, 'income': 10, 'score': 5000, 'cities': 5, 'kills': 10,
                 'casualties': 10, 'handicap': 2, 'opp_score': 5000}

N_TYPES = 40  # CommandType values are below 40
N_ARGS = 64


class Net(nn.Module):
    def __init__(self, d=256, layers=6, heads=8, width=14):
        super().__init__()
        self.width, self.tiles = width, width * width
        self.cat_names = list(TILE_CATEGORICAL)
        self.cat_offsets = []
        total = 0
        for n in self.cat_names:
            self.cat_offsets.append(total)
            total += TILE_CATEGORICAL[n]
        self.register_buffer('cat_idx', torch.tensor([T[n] for n in self.cat_names]))
        self.register_buffer('cat_off', torch.tensor(self.cat_offsets))
        self.register_buffer('cat_max', torch.tensor([TILE_CATEGORICAL[n] - 1 for n in self.cat_names]))
        self.tile_cat = nn.EmbeddingBag(total, d, mode='sum')
        n_dense = 16 * len(TILE_BITS) + len(TILE_SCALED) + 3 * len(TILE_POINTERS)
        self.tile_dense = nn.Linear(n_dense, d)
        self.pos = nn.Parameter(torch.randn(self.tiles, d) * 0.02)
        ys, xs = torch.meshgrid(torch.arange(width), torch.arange(width), indexing='ij')
        self.register_buffer('xy', torch.stack([xs.reshape(-1), ys.reshape(-1)], 1).float())

        self.g_cat_names = list(GLOBAL_CATEGORICAL)
        self.g_cat = nn.ModuleList(nn.Embedding(GLOBAL_CATEGORICAL[n], d) for n in self.g_cat_names)
        self.g_dense = nn.Linear(len(GLOBAL_SCALED) + 2 + polyenv.TECH_SLOTS, d)

        layer = nn.TransformerEncoderLayer(d, heads, 4 * d, dropout=0.0, activation='gelu', batch_first=True,
                                           norm_first=True)
        self.trunk = nn.TransformerEncoder(layer, layers)
        self.norm = nn.LayerNorm(d)

        self.cmd_emb = nn.Embedding(N_TYPES * N_ARGS, d)
        self.cmd_type = nn.Embedding(N_TYPES, d)
        self.no_tile = nn.Parameter(torch.zeros(2, d))
        self.cmd_a, self.cmd_b, self.cmd_g = nn.Linear(d, d), nn.Linear(d, d), nn.Linear(d, d)
        self.cmd_out = nn.Sequential(nn.LayerNorm(d), nn.Linear(d, d), nn.GELU(), nn.Linear(d, 1))
        self.value = nn.Sequential(nn.Linear(d, d), nn.GELU(), nn.Linear(d, 1))
        self.owner = nn.Linear(d, 3)

    def encode(self, tiles, glob, techs):
        """tiles int16 [B, 196, 32], glob int32 [B, 20], techs uint8 [B, 64] -> tokens [B, 197, d]."""
        B = tiles.shape[0]
        t = tiles.long()
        cat = torch.minimum(t[..., self.cat_idx].clamp(min=0), self.cat_max) + self.cat_off
        x = self.tile_cat(cat.reshape(-1, len(self.cat_names))).reshape(B, self.tiles, -1)
        dense = []
        for n in TILE_BITS:
            v = t[..., T[n]]
            dense.append(((v.unsqueeze(-1) >> torch.arange(16, device=t.device)) & 1).float())
        dense.append(torch.stack([t[..., T[n]].float() / s for n, s in TILE_SCALED.items()], -1))
        for n in TILE_POINTERS:
            p = t[..., T[n]]
            has = (p >= 0).float().unsqueeze(-1)
            target = self.xy[p.clamp(min=0)]
            here = (p == torch.arange(self.tiles, device=t.device)).float().unsqueeze(-1)
            dense += [has * (target - self.xy) / self.width, here]
        x = x + self.tile_dense(torch.cat(dense, -1)) + self.pos

        g = glob.long()
        gx = sum(emb(g[:, G[n]].clamp(0, emb.num_embeddings - 1)) for n, emb in zip(self.g_cat_names, self.g_cat))
        stars = glob[:, G['stars']].float()
        gd = torch.cat([torch.stack([glob[:, G[n]].float() / s for n, s in GLOBAL_SCALED.items()], -1),
                        torch.stack([torch.log1p(stars), torch.log1p(glob[:, G['income']].float().clamp(min=0))], -1),
                        techs.float()], -1)
        gx = gx + self.g_dense(gd)
        return self.norm(self.trunk(torch.cat([gx.unsqueeze(1), x], 1)))

    def forward(self, tiles, glob, techs, legal, legal_mask):
        """legal int16 [B, N, 4] padded, legal_mask bool [B, N]. Returns policy logits [B, N] (padding
        at -inf), value logit [B], ownership logits [B, 196, 3]. Only the real rows are scored."""
        h = self.encode(tiles, glob, techs)
        hg, ht = h[:, 0], h[:, 1:]
        bi, ni = legal_mask.nonzero(as_tuple=True)
        L = legal[bi, ni].long()
        ty = L[:, 0].clamp(0, N_TYPES - 1)
        e = self.cmd_emb(ty * N_ARGS + L[:, 3].clamp(0, N_ARGS - 1)) + self.cmd_type(ty)
        # Project the tile tokens once, then gather: the source and target tiles of each row.
        pa = torch.cat([self.cmd_a(ht), self.cmd_a(self.no_tile[0]).expand(ht.shape[0], 1, -1)], 1)
        pb = torch.cat([self.cmd_b(ht), self.cmd_b(self.no_tile[1]).expand(ht.shape[0], 1, -1)], 1)
        none = self.tiles
        a = torch.where(L[:, 1] >= 0, L[:, 1], none)
        b = torch.where(L[:, 2] >= 0, L[:, 2], none)
        z = e + pa[bi, a] + pb[bi, b] + self.cmd_g(hg)[bi]
        scores = self.cmd_out(z).squeeze(-1).float()
        logits = torch.full(legal_mask.shape, float('-inf'), device=scores.device)
        logits[bi, ni] = scores
        return logits, self.value(hg).squeeze(-1).float(), self.owner(ht).float()
