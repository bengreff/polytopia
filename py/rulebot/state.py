"""The bot's view of the game: what its own client shows (an Obs for its own player id) plus the
legal-command list. Nothing here reads the true game state."""
import numpy as np

import polyenv
from . import data as D

_F = {n: i for i, n in enumerate(polyenv.TILE_FEATURES)}


class Unit:
    __slots__ = ('tile', 'type', 'hp', 'maxhp', 'moved', 'attacked', 'passenger', 'owner', 'vet', 'xp', 'home', 'st')

    def __init__(self, tile, f, owner):
        self.tile = tile
        self.type = int(f[_F['unit']]) - 1
        self.hp = int(f[_F['unit_health']])
        self.maxhp = max(1, int(f[_F['unit_max_health']]))
        self.moved = bool(f[_F['unit_moved']])
        self.attacked = bool(f[_F['unit_attacked']])
        self.passenger = int(f[_F['unit_passenger']]) - 1
        self.vet = bool(f[_F['unit_promoted']])
        self.xp = int(f[_F['unit_xp']])
        self.home = int(f[_F['unit_home']])
        self.owner = owner
        self.st = D.stats(self.type)

    @property
    def naval(self):
        return self.type in D.NAVAL

    @property
    def land_type(self):
        """For a boat: the unit it carries (what it becomes on land)."""
        return self.passenger if self.naval and self.passenger >= 0 else self.type

    def __repr__(self):
        x, y = D.xy(self.tile)
        return f'{D.UNIT_NAME.get(self.type, self.type)}@({x},{y}){self.hp}'


class City:
    __slots__ = ('tile', 'owner', 'level', 'pop', 'capital', 'border', 'wall', 'rewards', 'connected', 'production')

    def __init__(self, tile, f):
        self.tile = tile
        self.owner = int(f[_F['owner']])  # the tile's owner (imp_owner is 0 on cities)
        self.level = int(f[_F['imp_level']])
        self.pop = int(f[_F['imp_population']])
        self.capital = int(f[_F['capital_of']]) != 0
        self.border = int(f[_F['imp_border']])
        self.rewards = int(f[_F['imp_rewards']])
        self.wall = bool(self.rewards & (1 << D.R_WALL))
        self.connected = int(f[_F['imp_connected']])
        self.production = int(f[_F['imp_production']])

    def __repr__(self):
        x, y = D.xy(self.tile)
        return f'city({x},{y})L{self.level}p{self.pop}' + ('*' if self.capital else '')


class View:
    """One decision's view: observation arrays, unit and city lists, and the legal list indexed."""

    def __init__(self, obs, legal):
        self.obs = obs
        t = obs.tiles.reshape(D.N, -1)
        self.t = t
        g = lambda n: obs.get(n)
        self.turn, self.me, self.tribe, self.stars, self.income = g('turn'), g('me'), g('tribe'), g('stars'), g('income')
        self.ncities, self.opp_tribe, self.trigger, self.opp_known = g('cities'), g('opp_tribe'), g('trigger'), g('opp_known')
        self.relation = g('relation')
        self.techs = set(int(i) for i in np.nonzero(obs.techs)[0])
        self.explored = t[:, _F['explored']].astype(bool)
        self.terrain = t[:, _F['terrain']]
        self.owner = t[:, _F['owner']]
        self.resource = t[:, _F['resource']]
        self.imp = t[:, _F['improvement']]
        self.imp_owner = t[:, _F['imp_owner']]
        self.imp_level = t[:, _F['imp_level']]
        self.road = t[:, _F['road']]
        self.ruling = t[:, _F['ruling_city']]
        self.capital_of = t[:, _F['capital_of']]
        self.unit_type = t[:, _F['unit']] - 1  # -1: none
        self.unit_owner = t[:, _F['unit_owner']]
        self.my_units, self.enemy_units, self.unit_at = [], [], {}
        for i in np.nonzero(t[:, _F['unit']] > 0)[0]:
            i = int(i)
            o = int(t[i, _F['unit_owner']])
            u = Unit(i, t[i], o)
            self.unit_at[i] = u
            (self.my_units if o == 1 else self.enemy_units).append(u)
        self.my_cities, self.enemy_cities, self.villages, self.ruins, self.city_at = [], [], [], [], {}
        for i in np.nonzero(self.imp == D.CITY)[0]:
            i = int(i)
            c = City(i, t[i])
            self.city_at[i] = c
            if c.owner == 1:
                self.my_cities.append(c)
            elif c.owner == 0:
                self.villages.append(i)
            else:
                self.enemy_cities.append(c)
        self.ruins = [int(i) for i in np.nonzero(self.imp == D.RUIN)[0]]
        self.capital = next((c for c in self.my_cities if c.capital), None)
        self.enemy_capital = next((c for c in self.enemy_cities if c.capital), None)
        self._index_legal(legal)

    def _index_legal(self, legal):
        self.legal = legal
        self.moves, self.attacks, self.trains, self.builds, self.upgrades = {}, {}, {}, {}, {}
        self.research, self.captures, self.examine, self.recover, self.heal, self.promote = {}, {}, {}, {}, {}, {}
        self.disembark, self.rewards, self.peace_resp, self.other = {}, [], [], []
        self.end = None
        for k, (ty, a, b, arg) in enumerate(legal.tolist()):
            if ty == D.MOVE:
                self.moves.setdefault(a, {})[b] = k
            elif ty == D.ATTACK:
                self.attacks.setdefault(a, {})[b] = k
            elif ty == D.TRAIN:
                self.trains.setdefault(a, {})[arg] = k
            elif ty == D.BUILD:
                self.builds.setdefault(a, {})[arg] = k
            elif ty == D.UPGRADE:
                self.upgrades.setdefault(a, {})[arg] = k
            elif ty == D.RESEARCH:
                self.research[arg] = k
            elif ty == D.CAPTURE:
                self.captures[a] = k
            elif ty == D.RUINS:
                self.examine[a] = k
            elif ty == D.RECOVER:
                self.recover[a] = k
            elif ty == D.HEAL:
                self.heal[a] = k
            elif ty == D.PROMOTE:
                self.promote[a] = k
            elif ty == D.DISEMBARK:
                self.disembark[a] = k
            elif ty == D.REWARD:
                self.rewards.append((arg, k))
            elif ty == D.PEACE_RESP:
                self.peace_resp.append((arg, k))
            elif ty == D.ENDTURN:
                self.end = k
            else:
                self.other.append((k, ty, a, b, arg))

    # ---- helpers
    def has(self, tech_name):
        return D.T[tech_name] in self.techs

    def my_city_tiles(self):
        return {c.tile for c in self.my_cities}

    def in_my_border(self, i):
        return self.owner[i] == 1

    def land(self, i):
        return self.terrain[i] in (D.FIELD, D.FOREST, D.MOUNTAIN)

    def water(self, i):
        return self.terrain[i] in (D.WATER, D.OCEAN)

    def city_of_tile(self, i):
        r = int(self.ruling[i])
        return self.city_at.get(r)
