"""Distances over the known map for planning moves beyond one turn (the legal list gives this turn's
reachable tiles; these maps rank them)."""
import heapq

import numpy as np

from . import data as D

INF = 1e9


def land_cost_array(view, climbing):
    """Cost to enter each tile for a land unit, by what the bot's client shows. Unexplored tiles get
    a mild cost (unknown terrain), water is impassable except our own ports (embark)."""
    ter = view.terrain
    cost = np.full(D.N, INF)
    for i in range(D.N):
        if not view.explored[i]:
            cost[i] = 1.6
            continue
        t = ter[i]
        if t == D.FIELD:
            cost[i] = 1.0
        elif t == D.FOREST:
            cost[i] = 2.0
        elif t == D.MOUNTAIN:
            cost[i] = 2.0 if climbing else INF
        elif view.imp[i] == D.PORT and view.owner[i] == 1:
            cost[i] = 2.0
    return cost


def water_cost_array(view, ocean_ok):
    cost = np.full(D.N, INF)
    for i in range(D.N):
        t = view.terrain[i]
        if not view.explored[i]:
            cost[i] = 1.5
        elif t == D.WATER:
            cost[i] = 1.0
        elif t == D.OCEAN:
            cost[i] = 1.0 if ocean_ok else INF
        elif view.imp[i] == D.CITY or (view.imp[i] == D.PORT):
            cost[i] = 1.0
    return cost


def dijkstra(sources, cost, roads=None, blocked=None):
    """Cost-distance from `sources` (tile list) to every tile: entering tile j costs cost[j] (0.5
    along roads). Returns a numpy array."""
    dist = np.full(D.N, INF)
    h = []
    for s in sources:
        dist[s] = 0.0
        h.append((0.0, s))
    heapq.heapify(h)
    neigh = D.NEIGH
    while h:
        d, i = heapq.heappop(h)
        if d > dist[i]:
            continue
        for j in neigh[i]:
            c = cost[j]
            if c >= INF:
                continue
            if roads is not None and roads[i] and roads[j]:
                c = 0.5
            if blocked is not None and j in blocked:
                c += 3.0
            nd = d + c
            if nd < dist[j]:
                dist[j] = nd
                heapq.heappush(h, (nd, j))
    return dist
