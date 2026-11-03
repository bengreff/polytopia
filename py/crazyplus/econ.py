"""Opening economy: alternatives to the AI's research, training and building, scored by a rollout of
several whole turns (the AI on both sides after the alternative, the opponent being the exact
opponent model), so a choice is judged by what it leads to a few turns later."""
from rulebot import data as D

BUILD, TRAIN, RESEARCH, ENDTURN = 1, 5, 8, 15
ECON = (BUILD, TRAIN, RESEARCH)
# improvements worth trying as alternatives (population, or the port that opens the water)
USEFUL_BUILDS = {D.FARM, D.FISHING_HUT, D.HUNTING_HUT, D.LUMBERHUT, D.MINE, D.HARVESTFRUIT, D.PORT, D.WINDMILL,
                 D.FORGE, D.SAWMILL, D.MARKET, D.STARFISHING}


def is_econ(ai_row, stars):
    if ai_row is None:
        return False
    return ai_row[0] in ECON or (ai_row[0] == ENDTURN and stars >= 5)


def candidates(legal, ai_row, max_n=12):
    """[(seq, reason)]: every research and training option, and the useful builds."""
    out = []
    seen = {ai_row}
    res, tr, bu = [], [], []
    for r in legal.tolist():
        r = tuple(int(x) for x in r)
        if r in seen:
            continue
        if r[0] == RESEARCH:
            res.append(([r], 'research ' + D.TECH_NAME.get(r[3], str(r[3]))))
        elif r[0] == TRAIN:
            tr.append(([r], 'train ' + D.UNIT_NAME.get(r[3], str(r[3]))))
        elif r[0] == BUILD and r[3] in USEFUL_BUILDS:
            bu.append(([r], 'build ' + D.IMP_NAME.get(r[3], str(r[3]))))
        seen.add(r)
    # interleave so that every kind is tried when there are many
    k = 0
    while len(out) < max_n and (res or tr or bu):
        for lst in (res, tr, bu):
            if lst and len(out) < max_n:
                out.append(lst.pop(0))
        k += 1
    return out
