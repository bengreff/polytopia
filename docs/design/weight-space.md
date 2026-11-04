# Weight-space programming for the agent (plan v2, 2026-11-04)

**Verdict: the MIT post overstates what it shows, but a narrower, real version is worth one
experiment here: find a specific defect in a trained network by measurement, fix it with a small
hand-made edit, and prove the fix in games.** The defect to look for first is in the critic, the
value network that scores the planner's candidate turns. The closest published case (a card-game DQN)
gained about 4 points of win rate that way. Writing networks from scratch and reading our 5.9M
weights directly are out.

This replaces v1 (2026-11-02), which repeated the post's framing too readily. v1 also led with weight
arithmetic, which isn't weight-space programming, and its "low-rank change" finding is weak evidence
(see section 3).

## 1. What the MIT post claimed, and what holds up

Takehi, Teoh, Zhang and Khattab (MIT CSAIL), "Sparks of Weight-Space Programming", a blog post of
2026-11-02 with logs. It is not peer-reviewed. All numbers here are as reported by the sources.

| Claim | What it really was | Holds up? |
|---|---|---|
| An LLM reads what a network computes from its raw weights | Tiny MLPs only: 17,253 and 171,664 weights. Named the game in 4 of 4 | **Yes, reproduced.** The Turing Institute replication: Astra 12/12, Opus 5.5 12/12; both fall to chance on shuffled weights. Models before mid-2026 fail |
| It fixed a car policy by editing weights | 171,664 weights, no forward passes: 0/256 → 1,024/1,024 successes | Not independently re-run that I found |
| It writes game networks from scratch that beat PPO | ~19k-weight MLPs. Breakout 517 vs PPO-500M's 25.3, but it **lost** on Seaquest (53 vs 89) and Asterix (53 vs 55). PPO on MinAtar is a weak baseline | **Partly.** Turing replication: Astra 457 / 25.5 / 66.2, **Opus 5.5 112 / 0 / 45.1**. Breakout has a simple program (track the ball) |
| It wrote a 1.5M-weight 9×9 Go network | Python sandbox and 0–50 batches of forward passes; the opponents were small 243→32→32→82 MLPs | A low bar (the critique) |
| It raised Qwen3.5-0.8B's MATH-500 from 7.9% to 48.1% by editing 0.016% of weights | **A loop fix**: 93% of answers hit the 32,768-token cap. It removed one "repetition direction" from two MLP layers (activation steering made permanent), with Python and 50 forward-pass batches | **The edit is real** (a rerun gave 51.0%), but the authors confirm the model already had the ability. On Gemma 3 270M, which rarely loops, it gained nothing |

The critique (Honnibal, "The 'Astra can read and write weights to improve Qwen' post is misleading")
makes three points, and I agree with them:
- "Every experiment involving a real language model uses Python." The headline results are an LLM
  doing an ML engineer's job (probe, find a direction, edit, test), not reading weights natively.
- The baselines flatter the writing results.
- What is real is still new: "AI models of a year or two ago would have failed at all these tasks."

**The closest case to ours:** Claude Opus edited a trained DQN champion for a golf card game
(vgainullin/golf, experiment 16).
- **Method:** self-consistency checks with forward-pass budgets and fixed random deals found two
  systematic valuation errors.
- **Result** (24,000 held-out matches): win rate 38.9% → 42.8% with 1 weight changed, 43.2% with
  22,876 changed.
- A second round added almost nothing (42.9%).
- So: modest, real gains from fixing how a value network misjudges positions, then diminishing
  returns.

## 2. What that means for us

**What carries over:**
- **The method:** measure the network's behaviour on chosen inputs, find a localized error, patch it
  in closed form, check it on held-out data. That is interpretability-guided editing, with games as
  the test budget.
- **The targets:** defects (obstacles), not missing abilities. Gemma's failure and the golf round 2
  both say an edit can't add skill.

**What doesn't:**
- **Reading our network directly.** It has 5.9M weights in a 6-layer transformer, 34× the largest
  network read raw. All reading here goes through forward passes and probes.
- **Writing from scratch.** Opus 5.5, which would do the work, writes much worse networks than Astra
  even at 20k weights.

**Why the critic first:**
- The planner is our largest measured gain: 82–94% against unplanned networks (S9 read). It ranks 16
  candidate turns by the critic's estimate after the opponent's reply.
- A systematic critic error (e.g. valuing banked stars too highly, or tech too little) biases every
  planned turn the same way. Through s10x it also biases training.
- The habits that no training run fixed are the natural suspects:

  | Indicator (gen 83) | Agent | Humans |
  |---|---|---|
  | Stars banked | 5.6–6.5 | 3.1 |
  | Techs by turn 20 | 6.6–7.2 | 9.2 |
  | Catapults (share of units) | 16–20% | 5% |

- Critic errors are cheap to find with forward passes alone:
  - calibration by phase, tribe, stars and fog;
  - consistency across a turn: the estimate at the start of our turn against the one after the reply;
  - counterfactuals: change the stars, techs or units in an observation and see how the estimate
    moves, against what those changes are worth in real outcomes.

**The policy's habits come second.** Unlike critic errors, they may be correct against self-play
opponents while losing to humans. That is the trap in a self-play bar.

## 3. What v1 got wrong

- **Weight arithmetic isn't weight-space programming.** Extrapolating along training's direction
  (ExPO) or summing the S10 tracks' changes may still help (the S9 soup reached 55%). But it is a
  cheap side test, with no judgement in it.
- **The gen 0 → 83 diff is weak evidence.** I measured that self-play's change is concentrated in a
  few directions in the value head (46% in the top one) and the last layer (40%). Fine-tuning updates
  are low-rank in general (the reason LoRA works), so this says little about where defects live.

## 4. The experiment: WSP-1 (pre-registered in the research log)

One Opus agent acts as the weight-space programmer, as in the MIT and golf setups: a Python sandbox on
the PC, a declared budget of forward passes and games, and every edit logged.

**Base:** `s9v` gen 83 with its fog critic `critic_g083.pt`. That is S10's base, and it has the
measured planner results.

**Part A: the critic.**
1. **Audit, offline:** on held-out self-play and human positions, run
   - calibration by phase, tribe, stars and fog;
   - the within-turn consistency check;
   - counterfactual probes on stars, techs and unit kinds.
2. **Rank the errors** by size and by how often the planner's choice depends on them (positions where
   the edited and unedited critic pick different plans).
3. **Edit:** a closed-form fix of the largest error, e.g. a bias or a direction in the critic's input
   or value head, the golf and Qwen kinds of edit. Screen it offline: held-out log loss must not get
   worse.
4. **Gate:** the planner with the edited critic against the planner with the original, on paired
   seeds, sides swapped. The budget and settings are fixed before it runs.

**Part B: one policy habit.** Star banking, with the same audit, edit and gate. The bar is Ben's:
≥ 55% vs the unedited network over 400 games, plus no drop against gen 0 and CrazyPlus.

**Control:** one ExPO extrapolation (α = 0.25) through the same raw gate. It is the "no judgement"
weight change, to compare the edits with.

**Hygiene:**
- Tune on one seed block and gate on a fresh one.
- At most 5 game gates in total.
- Every edit tried goes in the log, failures included.

**Expected (guessed):** a critic error of a few points of calibration in banking- or tech-heavy
positions, and a +2–5-point planner gain from fixing it. That is below Ben's 55% bar, so the result
will most likely be reported, not adopted.

**Kill if:**
- the audit finds no systematic error that changes the planner's choice on ≥ 5% of searched turns; or
- no gate reaches 52%.

## Sources

- Takehi, Teoh, Zhang, Khattab, [Sparks of Weight-Space Programming](https://rikiyat.github.io/blog/weight-space-programming/) (blog, 2026-11-02);
  logs: [RikiyaT/lm_understand-logs](https://github.com/RikiyaT/lm_understand-logs).
- Honnibal, [The 'Astra can read and write weights to improve Qwen' post is misleading](https://honnibal.dev/blog/astra-read-write-misleading);
  [Hacker News thread](https://news.ycombinator.com/item?id=50341174).
- Replications:
  - [alan-turing-institute/weights_editing](https://github.com/alan-turing-institute/weights_editing)
    (Astra, Opus 5.5 and older models; shuffled-weight controls);
  - [nihilistau/weight-space-claude](https://github.com/nihilistau/weight-space-claude);
  - [Krentsel's reproduction](https://officechai.com/ai/ability-for-weight-space-programming-has-appeared-only-in-latest-models-says-google-researcher-alex-krentsel-who-reproduced-results/);
  - [the authors' clarification of the loop fix](https://huggingnews.com/ai/replication-confirms-mit-weight-editing-study-as-authors-clarify-loop-fi-cd8a8f15).
- The game-agent case: [vgainullin/golf, experiment 16](https://github.com/vgainullin/golf/pull/24).
- Method kit: [lorenzofamiglini/weight-surgery](https://github.com/lorenzofamiglini/weight-surgery).
- Earlier steering and extrapolation work:
  - Mini et al. 2023, [Understanding and Controlling a Maze-Solving Policy Network](https://arxiv.org/abs/2310.08043);
  - Zheng et al. 2024, [ExPO](https://arxiv.org/abs/2404.16792).
- The v1 weight diff: `py/wsp/diff.py`.
