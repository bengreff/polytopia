"""Economy: research choice, improvement building, city rewards.

The research priorities start from the human championship games (data/human1: which techs the
winners of each tribe researched by turn 10 and 20, see docs/design/rulebot.md) and are scaled by
what the bot's territory actually holds (fruit, game, fish, crops, metal, forest)."""
from . import data as D

T = D.T

# Opening book: per tribe, the human winners' early techs in order (most common first-four
# sequences and turn-10 shares, data/human1). Earlier = more preferred.
BOOK = {
    D.TRIBE['aimo']: ['hunting', 'forestry', 'climbing', 'riding', 'organization', 'fishing', 'roads', 'mining'],
    D.TRIBE['bardur']: ['organization', 'forestry', 'fishing', 'riding', 'climbing', 'mathematics', 'roads'],
    D.TRIBE['hoodrick']: ['hunting', 'forestry', 'mathematics', 'riding', 'fishing', 'roads'],
    D.TRIBE['imperius']: ['riding', 'fishing', 'roads', 'hunting', 'climbing', 'forestry'],
    D.TRIBE['kickoo']: ['hunting', 'forestry', 'organization', 'sailing', 'riding', 'mathematics'],
    D.TRIBE['luxidoor']: ['hunting', 'fishing', 'forestry', 'organization', 'riding', 'climbing'],
    D.TRIBE['oumaji']: ['organization', 'farming', 'roads', 'fishing', 'climbing'],
    D.TRIBE['quetzali']: ['organization', 'hunting', 'fishing', 'forestry', 'climbing', 'archery'],
    D.TRIBE['vengir']: ['mining', 'hunting', 'forestry', 'climbing', 'fishing', 'riding'],
    D.TRIBE['xinxi']: ['mining', 'smithery', 'hunting', 'fishing', 'riding', 'forestry'],
    D.TRIBE['yadakk']: ['organization', 'fishing', 'riding', 'farming', 'hunting'],
    D.TRIBE['zebasi']: ['construction', 'riding', 'fishing', 'roads', 'hunting'],
}

# Pop-per-star builds (improvement -> (cost, pop))
POP_BUILDS = {D.HARVESTFRUIT: 1, D.HUNTING_HUT: 1, D.FISHING_HUT: 1, D.FARM: 2, D.MINE: 2, D.LUMBERHUT: 1, D.PORT: 1}


def territory_counts(v):
    """Unworked resources and terrain inside my borders."""
    c = dict(fruit=0, game=0, fish=0, crop=0, metal=0, forest=0, water=0, mountain=0, field=0, farm=0, mine=0, lumber=0,
             windmill_spot=0)
    for i in range(D.N):
        if v.owner[i] != 1:
            continue
        r, ter, imp = v.resource[i], v.terrain[i], v.imp[i]
        if imp == 0:
            if r == D.FRUIT:
                c['fruit'] += 1
            elif r == D.GAME:
                c['game'] += 1
            elif r == D.FISH:
                c['fish'] += 1
            elif r == D.CROP:
                c['crop'] += 1
            elif r == D.METAL:
                c['metal'] += 1
        if ter == D.FOREST and imp == 0:
            c['forest'] += 1
        if ter == D.WATER:
            c['water'] += 1
        if ter == D.MOUNTAIN:
            c['mountain'] += 1
        if imp == D.FARM:
            c['farm'] += 1
        if imp == D.MINE:
            c['mine'] += 1
        if imp == D.LUMBERHUT:
            c['lumber'] += 1
    return c


# What each tech unlocks for the economy: (resource or terrain key, pop per tile, build cost)
TECH_ECON = {
    'organization': [('fruit', 1, 2)],
    'hunting': [('game', 1, 2)],
    'fishing': [('fish', 1, 2), ('port', 1, 7)],
    'farming': [('crop', 2, 5)],
    'mining': [('metal', 2, 5)],
    'forestry': [('forest', 1, 3)],
    'construction': [('windmill', 1, 5)],
    'mathematics': [('sawmill', 1, 5)],
    'smithery': [('forge', 2, 5)],
}
POP_STARS = 5.0  # a population point's worth early on (a level is +1 star a turn plus a reward)


MIL_VALUE = {  # star value of a tech's units/abilities for the war, by turn (early, from turn 8)
    'riding': (8, 5), 'archery': (5, 10), 'shields': (4, 7), 'smithery': (6, 16), 'mathematics': (4, 15),
    'chivalry': (4, 16), 'freespirit': (1, 3), 'roads': (2, 4), 'sailing': (0, 3), 'ramming': (0, 4),
    'navigation': (0, 3), 'philosophy': (0, 4), 'trade': (0, 2), 'construction': (0, 1), 'climbing': (2, 2),
}
UNLOCKS = {'mining': 'smithery', 'hunting': 'archery', 'forestry': 'mathematics', 'freespirit': 'chivalry',
           'organization': 'shields', 'riding': 'freespirit', 'fishing': 'ramming'}


def tech_values(bot, v):
    """Star value of each researchable tech over the next ~10 turns: population it unlocks inside
    my borders (minus build costs), its units' worth for the war (more once the war is on), half
    the value of the tech it leads to, and the human opening book."""
    tc = territory_counts(v)
    tc['port'] = 1 if tc['water'] else 0
    tc['windmill'] = min(tc['farm'], 3)
    tc['sawmill'] = min(tc['lumber'] // 2, 2)
    tc['forge'] = min(tc['mine'], 2)
    book = BOOK.get(v.tribe, [])
    turn = v.turn
    war = bot.mem.get('contact_turn') is not None
    late = turn >= 8 or war
    th = bot.threat_level
    have = {D.TECH_NAME.get(t) for t in v.techs}
    have_mil = bool(have & {'archery', 'shields', 'smithery', 'mathematics', 'chivalry'})

    def econ_of(name):
        e = 0.0
        for key, pop, bcost in TECH_ECON.get(name, []):
            n = min(tc.get(key, 0), 4)
            e += n * (pop * POP_STARS - bcost)
        return max(0.0, e)

    def mil_of(name):
        a, b = MIL_VALUE.get(name, (0, 0))
        m = b if late else a
        if name == 'shields' and th > 1:
            m += 6
        if name == 'archery' and th > 0:
            m += 3
        if not have_mil and late and name in ('archery', 'shields', 'smithery', 'mathematics'):
            m += 8
        return m

    vals = {}
    for tech in v.research:
        name = D.TECH_NAME.get(tech, '')
        if name in ('meditation', 'spiritualism', 'diplomacy', 'aquatism'):
            vals[tech] = 0.0
            continue
        val = econ_of(name) + mil_of(name)
        nxt = UNLOCKS.get(name)
        if nxt and nxt not in have:
            val += 0.5 * (econ_of(nxt) + mil_of(nxt))
        if name == 'climbing' and 'mining' not in have:
            val += 0.3 * (econ_of('mining') + 0.5 * mil_of('smithery'))
        if name in book[:4] and turn < 12:
            val += 6.0 - 1.2 * book.index(name)
        vals[tech] = val
    return vals


def research_scores(bot, v):
    lit = T['philosophy'] in v.techs
    ncity = max(1, len(v.my_cities))
    return {t: val - D.tech_cost(D.TECH_TIER.get(t, 1), ncity, lit) for t, val in tech_values(bot, v).items()}


def choose_research(bot, v, budget, ratio=None):
    """(row, tech, cost) of the tech to research now, or None: the best value-minus-price tech
    whose value is at least `ratio` x its price."""
    if not v.research:
        return None
    ratio = bot.p['research_ratio'] if ratio is None else ratio
    lit = T['philosophy'] in v.techs
    ncity = max(1, len(v.my_cities))
    best = None
    for tech, val in tech_values(bot, v).items():
        cost = D.tech_cost(D.TECH_TIER.get(tech, 1), ncity, lit)
        if cost > budget or val < ratio * cost:
            continue
        if best is None or val - cost > best[0]:
            best = (val - cost, tech, cost)
    if best is None:
        return None
    return v.research[best[1]], best[1], best[2]


def build_value(v, tile, imp):
    """Rough star-equivalent value of building `imp` on `tile` now (pop is ~2.5 stars early)."""
    if imp in (D.HARVESTFRUIT, D.HUNTING_HUT, D.FISHING_HUT):
        pop = 1
    elif imp in (D.FARM, D.MINE):
        pop = 2
    elif imp == D.LUMBERHUT:
        pop = 1
    elif imp == D.PORT:
        pop = 1.6
    elif imp == D.WINDMILL:
        pop = sum(1 for j in D.NEIGH[tile] if v.imp[j] == D.FARM and v.owner[j] == 1)
    elif imp == D.SAWMILL:
        pop = sum(1 for j in D.NEIGH[tile] if v.imp[j] == D.LUMBERHUT and v.owner[j] == 1)
    elif imp == D.FORGE:
        pop = 2 * sum(1 for j in D.NEIGH[tile] if v.imp[j] == D.MINE and v.owner[j] == 1)
    elif imp == D.MARKET:
        lv = sum(int(v.imp_level[j]) for j in D.NEIGH[tile] if v.imp[j] in (D.WINDMILL, D.FORGE, D.SAWMILL)
                 and v.owner[j] == 1)
        return 2.0 * lv
    elif imp == D.STARFISHING:
        return 8
    elif imp in (D.TEMPLE, D.FORESTTEMPLE, D.WATERTEMPLE, D.MOUNTAINTEMPLE):
        pop = 1
    else:
        return 0
    c = v.city_of_tile(tile)
    bonus = 0
    if c is not None:
        need = c.level + 1 - c.pop  # pop still needed for the next level (approx.)
        if 0 < need <= pop:
            bonus = 2.0  # levels the city up now
    return pop * 2.6 + bonus


def choose_build(bot, v, budget, min_ratio):
    """Best (row, imp, tile, cost, value) build affordable within budget whose value/cost >= min_ratio."""
    best = None
    for tile, imps in v.builds.items():
        for imp, row in imps.items():
            cost = D.IMP_COST.get(imp, 0)
            if imp in (D.ROAD, D.BRIDGE, D.CLEARFOREST, D.BURNFOREST, D.GROWFOREST):
                continue
            if imp in (D.TEMPLE, D.FORESTTEMPLE, D.WATERTEMPLE, D.MOUNTAINTEMPLE) and v.stars < 25:
                continue
            if cost > budget:
                continue
            val = build_value(v, tile, imp)
            if val <= 0:
                continue
            if imp == D.PORT:
                # a port or two for pop and boats; never a port per lake tile
                ports = sum(1 for j in range(D.N) if v.imp[j] == D.PORT and v.owner[j] == 1)
                if ports >= max(1, (len(v.my_cities) + 1) // 3):
                    continue
                c = v.city_of_tile(tile)
                if c is not None and any(v.imp[j] == D.PORT and v.ruling[j] == c.tile for j in range(D.N)):
                    continue
            ratio = val / max(cost, 1)
            if ratio < min_ratio:
                continue
            if best is None or ratio > best[4] / max(best[3], 1):
                best = (row, imp, tile, cost, val)
    return best


def choose_reward(bot, v):
    opts = dict(v.rewards)
    tile = int(v.legal[v.rewards[0][1]][1])  # the city leveling up
    c = v.city_at.get(tile)
    th = bot.city_threat(v, c) if c is not None else bot.city_threat_at_reward(v)
    capital = c is not None and c.capital
    if D.R_WORKSHOP in opts or D.R_EXPLORER in opts:
        unexplored = int((~v.explored).sum())
        if D.R_EXPLORER in opts and v.turn <= 5 and unexplored > 120 and not bot.mem.get('explorer'):
            bot.mem['explorer'] = True
            return opts[D.R_EXPLORER], 'explorer'
        return opts.get(D.R_WORKSHOP, opts.get(D.R_EXPLORER)), 'workshop'
    if D.R_WALL in opts or D.R_RESOURCES in opts:
        wall = th >= 1.5 or (capital and bot.p['capital_wall'] and
                             (bot.mem.get('contact_turn') is not None or v.turn >= bot.p['capital_wall_turn']))
        if wall and D.R_WALL in opts:
            return opts[D.R_WALL], f'wall (threat {th:.1f}{", capital" if capital else ""})'
        return opts.get(D.R_RESOURCES, opts.get(D.R_WALL)), 'resources'
    if D.R_POPGROWTH in opts or D.R_BORDER in opts:
        return opts.get(D.R_POPGROWTH, opts.get(D.R_BORDER)), 'popgrowth'
    if D.R_SUPER in opts or D.R_PARK in opts:
        return opts.get(D.R_SUPER, opts.get(D.R_PARK)), 'super unit'
    return v.rewards[0][1], 'first'
