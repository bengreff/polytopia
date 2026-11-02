> **About this report.**
> - **Source:** written 2026-11-02 by a research agent Claude launched at Ben's request ("ONE agent" for radical ideas). Claude reviewed it.
> - **Spot-checked:** the kevinz917 repo, README and omniscient-critic ablation (62% of decisive games vs the control's 48%), and arXiv 2606.23348 (Generals.io) and 2605.19235 (GAE falls short).
> - **Correction:** kevinz917's 48–0 vs Crazy was played on Small **Dryland** maps, Bardur mirror only, not Lakes.
> - **Still to check:** claims marked (unverified) or (not re-checked) below.

# Radical ideas for breaking the plateau (research report, 2026-11-02)

Scope: the World Championship format (1v1 Might, Lakes, Small 14×14, fog, 12 tribes, 2.17.3), exact rules
through polyhost, local hardware only, no hand-written evaluator, fog-correct play. Read for context:
superhuman-plan.md, evaluation.md, architecture.md, training-explained.md, research-log.md (from "S3 to gen 48"
to the end), universal-book.md Part 0.

Citation convention: every claim about prior work has a source. **(not re-checked)** means I cite it from
my own knowledge and did not open it in this session. **(unverified)** means the claim comes from a
secondary summary or snippet, or from a source whose numbers I could not confirm.

---

## 0. The headline finding: a directly comparable Polytopia project appeared on 2026-10-29

`kevinz917/superhuman-polytopia-bot` (GitHub, created 2026-10-29, Apache 2.0, public weights) describes itself as "the first
superhuman Polytopia bot, trained from scratch with self-play reinforcement learning"
([repo](https://github.com/kevinz917/superhuman-polytopia-bot),
[blog](https://thekevinz.com/blog/polytopia-bot); the blog page did not render for me, so everything below comes
from the repo's README and lab notes).
- **Scope:** Bardur only (one tribe), 1v1, ten map types including Lakes, 14×14, fog of war. The rules come from a
  Rust re-implementation ("Polytopia 2.17.3 … checked against 27,785 transitions recorded from the game"), not the
  game's own code.
- **"Superhuman" is unverified.** The README shows 48–0 against Crazy with search, 24–0 without, and
  self-referential Elo. I found no human matches in what I read.
- **Scale:** a 0.5M-parameter CNN, pure PPO from random weights, **7.7 billion samples**, about 20 h on 1×H100 plus
  CPU actors, "under $500".
- **Their ablations, which bear directly on this project:**
  - **An omniscient critic was "the largest single improvement".** In a 1-hour ablation from their c3 checkpoint,
    it was the only arm to beat c3: 54% of all games and 65% of decisive ones, against the control's 40% / 47%. It
    did that on fewer samples (80M vs 113M)
    ([readout](https://github.com/kevinz917/superhuman-polytopia-bot/blob/main/docs/ablation-v1/readout-2026-10-20.md)).
  - **One-turn determinized search with the opponent's reply:** +104 Elo on c3 and +50 on their champion. It
    costs 4.7 s per searched turn on 32 cores. **Depth 2–3 and 64 or 256 replies add nothing**: the optimizer's curse
    ([sweep](https://github.com/kevinz917/superhuman-polytopia-bot/blob/main/docs/search-sweep-findings-2026-10-21.md)).
  - **Distilling search into the policy did not help.** The students' raw play equalled the parent's, and search
    on top of them gained only +54 to +67 Elo against the parent's +104
    ([refit](https://github.com/kevinz917/superhuman-polytopia-bot/blob/main/docs/search-distillation-refit-2026-10-21.md)).
  - **Potential-based reward shaping hurt:** 37% vs the control's 40%.
  - **Credit on a turn clock** (discounting only at End Turn) beat credit per decision: 41.8% / 39.5% vs 31.6%
    against the parent in 1 h
    ([c6](https://github.com/kevinz917/superhuman-polytopia-bot/blob/main/docs/c6-credit-readout-2026-10-21.md)).
  - **A fog-of-war value head fitted afterwards with the policy frozen:** AUC 0.975 overall, 0.754 at turns 0–9,
    trained in 45 min
    ([search doc](https://github.com/kevinz917/superhuman-polytopia-bot/blob/main/docs/scratch-search-2026-10-23.md)).
- **Polytopia Bench**, from the same repo: 12 frontier LLMs each wrote a Rust bot in 40 minutes. Against the RL bot,
  those bots won **0.3–3%** of games
  ([benchmark README](https://github.com/kevinz917/superhuman-polytopia-bot/blob/main/benchmark/README.md)).

The memory note "PolytopAI ~1340 Elo is the strongest known" should be revisited. I found nothing new on
PolytopAI itself: a web search returned only the unrelated Polytopia-ELO-Bot Discord bot. This project is still,
as far as I found, the only one on the game's own code, in the full 12-tribe championship format, trained on
human championship games. The other prior art I checked: Tribes (Perez-Liebana et al., AIIDE 2020), a Java
re-implementation, where MCTS with pruning beats rule-based agents and RHEA, decisions average 54 legal actions,
and turn branching is ~10^7 early and ~10^23 late
([AIIDE](https://ojs.aaai.org/index.php/AIIDE/article/view/7438),
[pruning](https://ceur-ws.org/Vol-2862/paper27.pdf)); Polyfish
([GitHub](https://github.com/HenBOMB/Polyfish)); tribes-rl
([GitHub](https://github.com/pham-tuan-binh/tribes-rl)).

---

## 1. Why the project plateaued: what the evidence says (my analysis)

**1. The improvement operator is starved, not just tuned badly.**
- **Data volume:**
  - The main line has seen ~83 generations × ~1M decisions ≈ 0.1B self-play decisions.
  - kevinz917's champion used 7.7B, about 90× more, and its Elo was still rising in log-samples.
  - Ataraxos used ~163M games, against this project's ~0.12M on the main line
    ([Lacuna summary](https://lacuna.tiptreesystems.com/work/superhuman-ai-for-stratego-using-self-play-reinforcement), unverified).
  - The Generals.io superhuman agent trained four days on 16 H200s
    ([arXiv 2606.23348](https://arxiv.org/abs/2606.23348)).
  - In Hex, AlphaZero's Elo grows with log compute at a constant slope (Jones 2021,
    [arXiv 2104.03113](https://arxiv.org/abs/2104.03113)).
- **Step size:** each generation moves the policy KL ~0.015 at lr 3e-5, on the noise of 1,500 binary outcomes.
- **The exploiter evidence is the strongest clue.**
  - `s9x` had the easiest RL problem there is: a fixed opponent, no anchor, 8 generations. It never passed 55%.
  - Ben found a decisive hole in one game.
  - So the optimizer can't find improvements that do exist. A robust network is the less likely reading.
  - A stronger optimizer would tell the two apart: a search-based exploiter (idea A3).

**2. The critic is the noise floor of everything.**
- **The value head is weak:**
  - it explains ~14% of outcome variance;
  - it memorized outcomes from a correlated stream;
  - it overrated off-distribution plans (S6).
- **Same failure in AlphaGo:** its value network overfitted on successive positions of the same game, and the fix was
  30M positions, each from a separate game (Silver et al. 2016,
  [Nature](https://www.nature.com/articles/nature16961), not re-checked).
- **Fog adds noise that an observation-only critic can't remove.** AlphaStar's largest RL-algorithm effect was
  giving the critic the opponent's observations: best-response win rate 22% → 82% (Vinyals et al. 2019,
  [Nature](https://www.nature.com/articles/s41586-019-1724-z); figures as transcribed in kevinz917's
  [AlphaStar review](https://github.com/kevinz917/superhuman-polytopia-bot/blob/main/docs/alphastar-review-2026-10-21.md),
  not re-checked against the paper). kevinz917 found the same lever largest in Polytopia.
- **GAE adds noise of its own:**
  - With stochastic policies (temperature 0.5 here, ~20 sampled commands per turn), GAE has extra variance even
    with an exact critic (VRPO / "GAE falls short", 2026, [arXiv 2605.19235](https://arxiv.org/abs/2605.19235)).
  - The per-decision λ 0.97 is ≈ 0.54 per turn, so credit leans heavily on a critic that isn't good.
- **Fits S9:** the one S9 track that changed the critic was the closest to passing (~55% over gens 79–83).

**3. No calculation.**
- The network does one forward pass per command.
- Polytopia tactics are deterministic sequences: kill chains, retaliation, healing, order of moves.
- **S6 was the only search test, and it used a protocol now shown to fail:**
  - scoring before the opponent's reply;
  - temperature-1 lanes;
  - a value head that had memorized outcomes.
- The kevinz917 protocol (determinized worlds, reply included) gains +50 to +104 Elo, and its gains come from
  one-reply tactical blunders that search catches.

**4. The prior is both the floor and the ceiling.**
- The KL anchor to gen 0 was the only lever that held.
- **The strategic gaps never moved, on any track:**

  | Indicator | Humans | Network |
  |---|---|---|
  | Techs by turn 20 | 9.2 | 6.6–7.2 |
  | Stars banked | 3.1 | 5.6–6.5 |
  | Catapults | 5% | 16–20% |

  - The tech pace was inherited from R3 (cloning the game's AI).
- Small KL-limited PPO steps on a single outcome bit can't find multi-turn economic plans. That is AlphaStar's
  argument for injecting human strategy statistics: with them, supervised + KL went from 1,400 to 1,540 Elo
  (review above, Fig. 3e).

**5. Measurement can't see small steps.**
- A 200-game evaluation resolves ±7%.
- Generations cycle at 47–49% vs their predecessors, so a single pairing is noise. evaluation.md addresses this.

**Bottom line.** Three binding constraints: samples, critic noise, and no lookahead.
- The cheapest lever not yet pulled correctly: play-time search.
- The largest proven training lever not yet pulled: the privileged critic.
- The largest structural gap: throughput.

---

## 2. The ideas

Each idea lists: the idea, why it might work here, the closest prior work and its result, the cost on this PC
(28 threads, RTX 4070 Ti SUPER 16 GB shared, at most 2 GPU jobs), the main risks, and a falsifiable first
experiment. "PC-hour" means the whole PC for an hour.

### Family A: search

#### A1. One-turn determinized search with the opponent's reply, at play time ("S6 done right")
- **Idea:**
  - At turn start, build K = 16 lanes: the greedy turn plus 15 sampled at T 0.5–1.
  - Each lane continues through the opponent's whole reply turn, played by the policy from the opponent's own view
    of a determinized sample (op 14).
  - Score each lane with a fog-safe value after the reply, averaged over 2–4 determinizations; play the best lane.
  - A lane that captures the last enemy city is a proven win, a fact rather than a judgement.
- **Why here:**
  - It fixes all three of S6's flaws.
  - It needs no training change, and the determinizer and turn_search.py already exist.
  - It strengthens the Ben match directly.
  - Jones 2021: test-time compute substitutes for training compute (10× training ≈ 15× test-time).
- **Prior:**
  - kevinz917 TurnSearcher: +104 Elo on c3, +50 on the champion, 81% vs the previous best with search; no gain
    beyond depth 1 / 1 reply ([sweep](https://github.com/kevinz917/superhuman-polytopia-bot/blob/main/docs/search-sweep-findings-2026-10-21.md)).
  - SPARTA: search on a fixed blueprint improves it monotonically; Hanabi 24.08 → 24.61
    ([arXiv 1912.02318](https://arxiv.org/abs/1912.02318)).
- **Cost:**
  - 1–2 engineering days.
  - ~16 × 40 decisions ≈ 640 engine steps plus batched forward passes per searched turn: ~2–5 s (estimate).
  - A 200-game match ≈ 1–2 PC-hours.
- **Risks:**
  - The value is the weak link, so A2 may be needed.
  - The determinizer's kept-city leak inflates gains a little. Measure with and without hiding fogged cities.
- **First experiment:**
  - `s9v` gen 83 searched with its critic vs raw `s9v` gen 83, 200 games (100 seeds × 2 seats).
  - Repeat with `s9c`'s head on `s9c`.
  - Log the override rate and which raw losses are rescued.
  - **Kill if ≤ 53% with both scorers.** Then the value can't rank turns even with the reply included; do A2 first.

#### A2. A search-grade fog value: a frozen-policy head on the whole self-play archive, decorrelated
- **Idea:**
  - Freeze the policy and fit a separate fog-observation value network.
  - **Data:** the archive of all tracks' games (S3–S9: hundreds of thousands of games, if kept), only a few
    positions per game, shuffled across games (the AlphaGo fix).
  - **Targets:** the outcome, or TD(λ); better still, the omniscient critic's prediction (B1), since the fog value
    should equal E[V_omni | observation], a lower-variance target than the outcome.
  - Average over the 8 board symmetries at inference if C2 checks out.
- **Prior:**
  - kevinz917's frozen-policy fog head: AUC 0.975 (0.754 at turns 0–9) vs 0.719 untrained, 45 min of training.
  - AlphaGo's one-position-per-game value training (not re-checked).
  - "Stop regressing": classification value losses (HL-Gauss) scale better
    ([arXiv 2403.03950](https://arxiv.org/abs/2403.03950)).
- **Cost:** half a day of engineering; 2–6 GPU-hours.
- **Risk:** the archive spans many policies, so the value is policy-averaged. Weight recent generations more.
- **First experiment:**
  - Held-out log loss and AUC by phase (early/mid/late) on 150 held-out human games and held-out `s9c` games, vs
    `s9c`'s head and `s9v`'s critic; then A1 with it.
  - **Kill if** log loss isn't ≥ 0.02 better than `s9c`'s head and A1's gain doesn't rise.

#### A3. Search-based exploiters (victim-play) as the exploitability measure
- **Idea:**
  - Measure exploitability with A1's searcher, using the victim's frozen policy as the reply model (victim-play),
    optionally on top of a PPO-trained exploiter.
  - Collect the positions where it wins; they are "holes" for evaluation suites and start states.
- **Why here:**
  - PPO exploiters found nothing (≤ 55%) while Ben did.
  - This tells whether the network is robust or the optimizer weak, and it decides whether the league is worth
    continuing.
- **Prior:**
  - Adversarial policies with victim modelling beat superhuman KataGo > 97%; the pass-adversary used 0.13% of
    KataGo's training compute ([arXiv 2211.00241](https://arxiv.org/abs/2211.00241)).
  - ISMCTS-BR (search + RL best response) exploited AlphaZero-style agents
    ([arXiv 2004.09677](https://arxiv.org/abs/2004.09677)).
- **Cost:** about one day once A1 exists; 200 games ≈ 2 PC-hours.
- **First experiment:**
  - Search-exploiter vs `s9c` gen 83.
  - **> 65%** (while `e9c`'s PPO exploiter sits near 55%): PPO exploiters understate exploitability, so put search
    exploiters into the league and the panel.
  - **≤ 58%:** the network is genuinely hard to exploit at one turn of depth.

#### A4. Per-decision Gumbel search as an expert-iteration target
- **Idea:**
  - At a decision, take n = 8–16 legal rows without replacement (Gumbel top-k).
  - Complete each to the end of the turn plus the reply with policy rollouts; sequential halving.
  - Train the policy toward the Gumbel "completed-Q" improved policy, not the argmax of noisy lanes.
- **Why here:** kevinz917's distillation of max-over-lanes choices failed. Gumbel targets are a principled policy
  improvement, guaranteed in expectation, even with a few simulations.
- **Prior:**
  - Gumbel AlphaZero/MuZero ([OpenReview](https://openreview.net/forum?id=bERaNdoegnO)).
  - Multiagent Gumbel MuZero for combinatorial actions, up to 10× fewer environment interactions
    ([AAAI 2024](https://ojs.aaai.org/index.php/AAAI/article/view/29121)).
  - Expert Iteration (Anthony et al. 2017, [arXiv 1705.08439](https://arxiv.org/abs/1705.08439), not re-checked).
  - **Counter-evidence:** kevinz917's distilled students lost about half the search gain.
- **Cost:**
  - ~15× simulator time per searched decision; on 25% of turns, ~3–4× a generation's game time.
  - 3–5 engineering days.
- **Risks:** the optimizer's curse, critic noise, and a narrower sampling distribution for later search (as
  kevinz917 found).
- **First experiment (only after A1 and A2 pass):**
  - Label 2,000 self-play games with Gumbel targets on 25% of decisions; fine-tune with a KL anchor.
  - **Kill if** the student's raw play is < 52% vs the parent, or A1 on the student gains less than half of A1 on
    the parent.

#### A5. Test-time policy fine-tuning ("search = one damped RL update", the Ataraxos pattern)
- **Idea:**
  - At play time, from the current position, play M determinized continuations of 1–2 turns.
  - Compute advantages with the critic and take a few KL-damped policy-gradient steps on a copy of the network
    (last layers or LoRA) for this position only; then act.
  - It smooths the choice instead of picking the max of a few noisy lanes.
- **Prior:**
  - Ataraxos (Nature 2026; [arXiv 2511.07312](https://arxiv.org/abs/2511.07312)): policy-value network + belief
    network + test-time search that "simply performs an additional damped self-play RL update". 15-1-4 against the
    most decorated human player, at ~1/500 of DeepNash's compute
    ([MIT News](https://news.mit.edu/2026/game-playing-ai-stratego-new-champ-0930)).
  - Fickinger et al. 2021, RL fine-tuning for online planning ([arXiv 2109.15316](https://arxiv.org/abs/2109.15316);
    results unverified).
- **Cost:** seconds to tens of seconds per turn of GPU at play time only; 3–4 engineering days.
- **Risk:** latency against the clock; gradient noise; needs A2's value.
- **First experiment:** vs A1 at equal wall time per turn, 200 games. **Kill if ≤ 50%.**

#### A6. Turn-level evolutionary planning seeded by the policy (OEP)
- **Idea:**
  - A population of whole turns.
  - Mutation: resample the suffix after command k from the policy. Crossover: swap per-unit sub-sequences.
  - Fitness: the value after the reply.
- **Prior:**
  - Online Evolution beat MCTS variants "by a large margin" in Hero Academy
    ([Justesen et al. 2016](http://julian.togelius.com/Justesen2016Online.pdf),
    [TCIAIG 2017](https://sebastianrisi.com/wp-content/uploads/justesen_tciaig17.pdf)).
  - Evolutionary MCTS beat OEP ([Baier & Cowling 2018](https://dl.acm.org/doi/10.1109/CIG.2018.8490403)).
  - In Tribes, MCTS with pruning beat RHEA (Hsu & Perez-Liebana 2020).
- **Risk:** more optimization pressure on a noisy critic. kevinz917's depth sweep showed the apparent gain doubling
  while wins stayed flat.
- **Cost:** 3 engineering days.
- **First experiment:** only if A1 saturates (16 → 64 lanes adds < 2 points). OEP at equal engine steps vs
  64-lane A1. **Kill if ≤ 52%.**

#### A7. A learned belief model for determinization
- **Idea:**
  - A network trained by supervised learning on self-play ground truth (facts) predicts what's hidden: enemy units
    in fog (count and placement), enemy cities on unexplored tiles, stars, techs.
  - The determinizer samples from it instead of priors. That fixes the kept-city leak and samples units in the fog.
- **Prior:**
  - Ataraxos's belief network.
  - Learned Belief Search: a learned belief cut search cost at near-exact-belief strength in Hanabi
    ([arXiv 2106.09086](https://arxiv.org/abs/2106.09086)).
  - Perfect-information Monte Carlo sampling works well when hidden information disambiguates fast (Long et al.,
    "Understanding the Success of Perfect Information Monte Carlo Sampling in Game Tree Search", AAAI 2010; not
    re-checked).
- **Cost:** 2–3 engineering days (samples must become valid states through op 14); a few GPU-hours.
- **First experiment:**
  - Held-out log-likelihood of the true hidden enemy units, cities and stars vs the current priors; then A1's gain
    with learned vs prior beliefs.
  - **Kill if** A1's gain changes by < 2 points.

### Family B: learning paradigm

#### B1. Omniscient (privileged) critic, training only (Ben's call; the fog principle holds for the agent)
- **Idea:**
  - The critic sees both players' full observations (or the full state); the policy keeps its fogged view.
  - Its values drive GAE (better: turn-clock credit), and it later teaches A2's fog value.
  - To stay unbiased under partial observability, give the critic the policy's observation as well as the hidden
    state (Baisero & Amato 2022, "Unbiased Asymmetric Reinforcement Learning under Partial Observability",
    unverified).
- **Why here:**
  - The critic is the binding noise source.
  - S9's critic track (observation only) was already the closest to passing.
  - Two independent systems, one of them in this very game, found this the largest lever.
- **Prior:**
  - AlphaStar Fig. 3k: 22% → 82% (via the review above).
  - kevinz917: +14 points in 1 h, climbing 52 → 66% per 50 updates, the only arm that helped.
  - Suphx's oracle guiding: training with perfect information, gradually dropped, made learning much faster
    ([arXiv 2003.13590](https://arxiv.org/abs/2003.13590)).
  - Asymmetric actor-critic (Pinto et al. 2017, [arXiv 1710.06542](https://arxiv.org/abs/1710.06542), not re-checked).
- **Cost:**
  - polyhost emits the unfogged observation for the learner only (the allowlist builder with both viewers);
    `train_rl --critic` already exists.
  - 1–2 engineering days; +10 min of learning per generation (as `s9v`); 12 generations ≈ 2 days in a two-track
    rotation.
- **Risks:**
  - Ben's rule. The critic never acts; only advantage signals reach the policy.
  - Hidden-state leakage into the logged observations: test that the policy's inputs are unchanged.
- **First experiment:**
  - `s9v`'s recipe with omniscient critic inputs vs `s9c`, from gen 83, 8–12 generations.
  - **Better if > 55%** over the last 4 generations.
  - **Kill if < 52%** and the critic's held-out log loss isn't lower than `s9v`'s.

#### B2. A Q-head over legal rows and expected-SARSA advantages (Q-boosting / VRPO)
- **Idea:**
  - The pointer head already scores every legal row. Add a Q output per row (trained on the taken row, with
    omniscient inputs as in B1).
  - Back up with Σ_a π(a|s) Q(s,a) instead of sampled multi-step GAE, so the noise of the within-turn sampled
    commands averages out.
- **Prior:** VRPO ([arXiv 2605.19235](https://arxiv.org/abs/2605.19235)):
  - GAE variance persists even with an exact critic in stochastic self-play;
  - Q-boosting gave > 15% lower exploitability than every baseline on mid-sized games;
  - it beat PerfectDou in Dou Dizhu (+0.152);
  - it was positive against Slumbot.
- **Cost:** 2–3 engineering days.
- **Risks:** Q learning over 100–1,000 rows is unstable; complexity.
- **First experiment:** after B1, B1 + Q-boosting vs B1. **Kill if < 52%.**

#### B3. Throughput ×3–5, then sample-scaled PPO (an enabler)
- **Idea:**
  1. Measure engine-only decisions/s with a trivial policy on 28 native processes.
  2. Remove the inference bottleneck. A generation's games are GPU-bound at ~24 positions per batch; use 64–256
     game slots per shard and bigger batches.
  3. Move the learner toward kevinz917's regime: 262k-decision batches, 1 epoch, lr 1e-4 to 3e-4, the anchor kept,
     the per-generation KL early stop loosened, plus EMA (B4).
- **Why here:**
  - QEMU measured ~57 decisions/s per process, and native was 7.6× faster (5 s vs 38 s for 12 games). That
    suggests the CPU side could feed ~10k decisions/s (an estimate, to measure).
  - Today: 2,200/s.
  - If the plateau is partly starvation, more samples in the same days is the remedy (Jones 2021).
- **Prior:**
  - kevinz917's Elo curve from random weights: c3 passed at ~2.5 h, +442 at 20 h.
  - A smaller batch (65k) learned the same per sample
    ([pilots](https://github.com/kevinz917/superhuman-polytopia-bot/blob/main/docs/pilots-2026-10-23.md)).
- **Cost:**
  - 2–4 engineering days.
  - Even at 10k/s, 7.7B samples is ~9 days of the PC. Scaling is a multi-week investment, not a quick win.
- **Risks:**
  - The GPU's 2-job limit.
  - The legal-list cost in il2cpp (1.2–1.6 ms in emulation) may be the real ceiling.
  - Higher lr destabilized S2 (KL 1.6 in a generation), so raise it in steps.
- **First experiment:**
  - The engine-only benchmark, 1 hour.
  - **Kill the scaling story if < 6k decisions/s** natively on 28 processes (then attack legal-list cost or C1
    first).
  - Then 24 h at the new rate vs `s9c`.

#### B4. Weight averaging across cycling generations (soups/EMA), plus top-25% advantage filtering
- **Idea:**
  - Generations orbit a point (47–49% vs the predecessor), so average the last K generations' weights.
  - Deploy the average and use it as the pool's anchor; keep an EMA during training.
  - Separately: learn only from the top 25% of decisions by |advantage|.
- **Prior:**
  - Generals.io superhuman agent: EMA led the last iterate by ~30 Elo after six days; top-25% filtering's best seed
    led the next setting by 112 Elo at 24 h ([arXiv 2606.23348](https://arxiv.org/abs/2606.23348)).
  - Ataraxos filters to the top 25% of advantages (Lacuna summary, unverified).
  - Model soups (Wortsman et al. 2022, [arXiv 2203.05482](https://arxiv.org/abs/2203.05482), not re-checked).
  - **Counter-evidence:** kevinz917's filter + 1 epoch + EMA arm gained +4 points (within noise), but made the
    learner 8× cheaper.
- **Cost:** the soup test needs no training (a few PC-hours of games); the filter is a flag.
- **Risk:** weight averaging across generations assumes they're linearly connected. With KL ~0.015 per
  generation they probably are.
- **First experiment:**
  - Uniform soup of `s9c` gens 76–83 vs `s9c` gen 83 (400 games), and vs gen 0.
  - Also a soup of the four S9 tracks' gen 83 (same parent, gen 71).
  - **Kill if ≤ 50%** vs gen 83.

#### B5. Strategy-statistic conditioning (AlphaStar's z) from human build facts
- **Idea:**
  - **z** = facts from a human game: the first 6 techs in order, the first 10 units, the turn of the first
    port/ship, cities by turn 10.
  - **Imitation:** BC with z as an input, z dropped 10% of the time.
  - **Self-play:** 25–50% of games conditioned on a z sampled from the human games. No pseudo-reward, or AlphaStar's
    pseudo-rewards, which are Ben's call since they are shaping.
  - **At play time:** z = 0, or z chosen by A1 among a few candidates.
  - z also gives evaluation controllable style opponents.
- **Why here:**
  - It is the only idea aimed squarely at the gaps that no track moved and that Ben exploited: tech tempo, banking,
    catapults, navy.
  - It protects human strategy modes from self-play's narrowing.
- **Prior:**
  - AlphaStar Fig. 3e: + z, 1,400 → 1,540 Elo (via the review).
  - TacticCraft conditions DI-Star on LLM-classified build-order tactics
    ([arXiv 2507.15618](https://arxiv.org/abs/2507.15618), snippet only).
  - MiniRTS: a language latent between planner and executor beat direct imitation
    ([arXiv 1906.00744](https://arxiv.org/abs/1906.00744)).
- **Cost:** a fresh human pass (Ben's OK): ~1 GPU-day of BC, then a 2-day RL track; 2 engineering days.
- **Risks:**
  - RL may learn to ignore z without pseudo-reward.
  - Bundling it with a model-size change would confound the result, so keep the arms separate.
- **First experiment:**
  - BC with z: is it **controllable**? z with 9 techs by turn 20 should give ≥ 8.5; z with 0 catapults should give
    ≤ 5%.
  - Strength ≥ R7 vs Crazy +2★.
  - Then an RL track vs `s9c`.
  - **Kill if** the policy follows z for less than half of the gap.

#### B6. One fresh human pass: a larger model, offline-RL weighting and auxiliary fact heads (Ben's OK)
- **Idea:**
  - **Model:** 20–40M parameters.
  - **Data:** pretrain on the ~14k Might Continents games, then the 7.7k championship games, once.
  - **Two arms:** plain BC vs advantage-weighted BC (weights from a value learned on human outcomes, AWR/IQL-style).
  - **Auxiliary heads** predicting facts: opponent stars and techs, units in fog (training-time ground truth),
    next-turn ownership, the opponent's next command.
- **Prior:**
  - AlphaStar Unplugged: offline actor-critic and MuZero Unplugged beat BC; the best won 90% vs the published
    AlphaStar supervised agent ([arXiv 2308.03526](https://arxiv.org/abs/2308.03526)).
  - KataGo: auxiliary ownership and score targets "significantly accelerate learning"
    ([arXiv 1902.10565](https://arxiv.org/abs/1902.10565)).
  - Maia-2: skill-conditioned attention, ~2 points better move prediction
    ([arXiv 2409.20553](https://arxiv.org/abs/2409.20553)).
- **Cost:** 1–2 GPU-days per arm; 2 engineering days.
- **Risks:**
  - 3.17M decisions is small for 40M parameters: overfitting, value memorization.
  - The "train once" rule.
- **First experiment:**
  - 20M BC vs 20M AWR: held-out top-1 (R7 is 43.7%), head-to-head vs R7, and vs Crazy +2★.
  - **Kill if both ≤ 52%** vs R7.

#### B7. A from-scratch track with the omniscient critic (tests whether the human prior is a ceiling)
- **Idea:** random init, omniscient critic, turn-clock credit, big batches, no anchor; possibly a small CNN like
  kevinz917's.
- **Prior:**
  - **For:**
    - kevinz917's from-scratch champion beat its own imitation-bootstrapped lineage by +442 Elo;
    - Generals.io: "behavior cloning, potential-based reward shaping, and population-based self-play are not
      required at scale".
  - **Against:** AlphaStar without human data reached 149 Elo vs 1,540.
- **Cost:** needs B3; days to weeks of the PC.
- **Risks:**
  - 12 tribes and Lakes navy make exploration much harder than a Bardur mirror.
  - It would lose the human-style play that beats Crazy.
  - It competes for the GPU.
- **First experiment:** 48 h after B3. **Kill if** it's < 50% vs Crazy at 48 h, or its curve is flat over the last
  12 h.

### Family C: architecture

#### C1. Encode once per turn, decode the turn autoregressively
- **Idea:**
  - The 6-layer trunk runs at turn start (and after kills or reveals).
  - A small decoder emits the turn's commands as pointers over the game's legal rows, attending to the trunk's tile
    tokens plus tokens for the commands already played and their outcomes (damage, kills, new legal rows).
- **Why here:**
  - It cuts trunk compute ~5–15× per turn, the B3 lever on the GPU side.
  - It models a turn as one plan, so combinations get learned jointly.
- **Prior:**
  - AlphaStar's autoregressive action heads (not re-checked).
  - kevinz917's family → source → target head.
  - Gym-μRTS GridNet: per-cell actions plus masking reached 91% and beat the 2020 μRTS champion on one GPU in 60 h
    ([arXiv 2105.13807](https://arxiv.org/abs/2105.13807)).
- **Cost:** 4–6 engineering days plus a BC run.
- **Risk:** the board changes inside the turn and the decoder sees it only through outcome tokens, so accuracy may
  fall.
- **First experiment:** BC on the human set at equal parameters. **Kill if** top-1 drops > 2 points or if it's
  < 3× faster per turn.

#### C2. The board's 8 symmetries: augmentation and test-time averaging
- **Idea:** if the rules are invariant under rotations and reflections, that is 8× data for the value and a free
  8-way ensemble for search scoring.
- **Prior:** AlphaGo, AlphaZero and KataGo augment with board symmetries (not re-checked). The project's
  architecture doc already proposes it; I found no use of it in `py/`.
- **Cost:** 1–2 days; it needs a state transformer in polyhost (the determinizer shows the format is understood).
- **Risk:** the engine's iteration orders, unit direction, map edges and tie-breaks may break exact invariance.
- **First experiment:** transform 1,000 states and compare legal lists modulo the transform, plus 2-turn playouts.
  **If > 1% differ:** augment approximately (value only) or drop it.

#### C3. Fog memory features (last seen)
- **Idea:** per-tile facts from the player's own observation history: when the tile was last seen, and the last
  enemy unit seen there with its type and turn.
- **Prior:** Generals.io's superhuman agent uses temporal tokens for the opponent's army and land over windows; OpenAI
  Five and AlphaStar use LSTMs (not re-checked).
- **Cost:** 1–2 days.
- **First experiment:** BC with vs without the features: held-out top-1 and value log loss by phase. **Kill if**
  < 0.5 points top-1 and no value gain.

### Family D: data and compute

#### D1. An independent external opponent: port kevinz917's public weights into the panel
- **Idea:**
  - Adapt their 14×14-plane observation and family/source/target action head to polyhost's observation and legal
    list.
  - Add their champion (with its fog value head, plus turn search) and hero-1v1 to the gauntlet, for Bardur cells.
- **Why here:** it is the only strong, independently trained Polytopia agent with public weights (Apache 2.0), and
  an anchor for the "superhuman" question that this project doesn't control.
- **Cost:** 2–4 days (adapter plus a parity check).
- **Risks:**
  - Their engine's known rule differences (their rules-alignment doc).
  - Bardur only.
  - Their search needs their value head.
- **First experiment:** an adapter parity check on 100 states. Then 200 Bardur-mirror games vs `s9c`, raw and with
  A1.

#### D2. More human data
- **Idea:** the 14k Might Continents games as pretraining, and any 2026 championship games, in B6's single pass.
- **Ben's call** (the no-memorizing rule).
- **Prior:** AlphaStar's supervised agent improved when fine-tuned on high-MMR games (87% → 96% vs the elite bot,
  via the review).

#### D3. Search-labelled human positions
- **Idea:** label the 53k human midgame positions with A1/A4-improved targets and distill them into the next model,
  as in "Grandmaster-level chess without search": a 270M transformer distilled from Stockfish action-values reached
  2,895 Lichess blitz Elo ([arXiv 2402.04494](https://arxiv.org/abs/2402.04494)).
- **Risk:** kevinz917's distillation failed, and the teacher here is much weaker than Stockfish.
- **Do it only if A4 succeeds.**

### Family E: LLM-adjacent (honest assessment: low value for the agent, some value for evaluation)

#### E1. LLM-written style probes for evaluation only
- **Idea:**
  - Use a frontier model (Claude, offline) or a local model to write scripted Python bots against polyhost, each
    with a forced style: navy-first, knight rush, wide expansion, tech rush.
  - They are evaluation opponents and probes, never the agent's judgement (Ben to confirm).
- **Prior:**
  - Polytopia Bench: frontier-model bots scored 13–75 on a six-opponent panel but won only 0.3–3% vs the RL bot.
    They are playable, not strong.
  - Play-style diversity in Tribes via quality-diversity search
    ([arXiv 2104.08641](https://arxiv.org/abs/2104.08641), title only).
- **Cost:** 1–2 days per few bots.
- **First experiment:**
  - Do the probes change the panel's ranking or expose a cell where the network does much worse than vs gen 0?
  - **Kill if** every probe scores < 5% against `s9c` and none correlates with Ben's results.

#### E2. LLM mining of human replays for strategy concepts (a local Qwen3-14B or gpt-oss-20b on the 4070 Ti)
- **Idea:**
  - Turn compact per-turn fact summaries of human games into text.
  - Have a local LLM label strategies (rush/navy/economy) and failure modes, to name or validate B5's z vocabulary
    and to cluster the network's losses.
- **Prior:**
  - TacticCraft used LLM-classified build orders (snippet only).
  - CivRealm: LLM agents collapse on tactics ("pirate invasions") in the full game
    ([arXiv 2401.10568](https://arxiv.org/abs/2401.10568)).
  - Concept discovery from AlphaZero's own representations taught grandmasters new ideas
    ([arXiv 2310.16410](https://arxiv.org/abs/2310.16410)). That is a non-LLM path to "concepts".
  - gpt-oss-20b runs in ~16 GB ([OpenAI](https://openai.com/index/introducing-gpt-oss/), not re-checked).
- **Cost:** 1–3 days; GPU contention with training.
- **Risk:** k-means on the same facts probably does as well.
- **First experiment:** LLM labels vs k-means clusters on 500 games. **Kill if** the LLM's labels predict outcome or
  z-controllability no better than the clusters.

#### E3. LLM as strategist with a learned tactician: see section 4 (advised against).

### Family F: other

#### F1. A mixed strategy over the S9 population at play time
- **Idea:** S9 produced four near-equal, different networks. Sample one per game (or per turn) from a Nash mixture
  fitted on their head-to-head matrix, or distill the mixture into one network.
- **Prior:** AlphaStar evaluates its league through the Nash distribution (via the review); PSRO (Lanctot et al. 2017,
  [arXiv 1711.00832](https://arxiv.org/abs/1711.00832), not re-checked).
- **Cost:** hours.
- **First experiment (needs A3):** exploitability of the mixture vs `s9c` alone. **Kill if** the difference is
  < 5 points.

---

## 3. Ranked shortlist: the five to try first, in this order

1. **A1 + A2: play-time one-turn determinized search with the opponent's reply, scored by a frozen-policy fog value
   trained on the archive.**
   - The best evidence in this exact game (+50 to +104 Elo).
   - No training change, no rule question, and the determinizer exists.
   - It answers the S9 planner re-test and strengthens the Ben match at once.
   - It then becomes the search-based exploiter (A3) and the later ExIt teacher.
   - Start with the S9 critic as the scorer; build A2 in parallel.
2. **B1: the omniscient critic** (Ben's call).
   - The largest training lever in two independent systems, one of them Polytopia.
   - S9's observation-only critic was already the closest track to passing.
   - It also supplies better targets for A2's fog value.
   - Run it as an S10 track vs the control, with turn-clock credit as a separate flag.
3. **B3: throughput ×3–5, then a sample-scaled PPO regime.**
   - The engine-only benchmark (an hour) tells whether the project is starved: ~0.1B decisions vs 7.7B for the bot
     that kept climbing.
   - If yes, every other training idea gets 3–5× more signal per day.
4. **B4: weight soup / EMA (plus top-25% advantage filtering).**
   - Hours and near-zero risk; run it while A1 is being built.
   - It directly tests "generations orbit a point".
   - A soup that wins becomes the deployed network and the anchor.
5. **B5: strategy-statistic (z) conditioning, in the next fresh human pass** (Ben's OK).
   - The only idea aimed at the strategic gaps every track left unchanged (tech tempo, banking, catapults, navy)
     and that Ben exploited.
   - It also yields controllable style opponents for evaluation.

**Order in practice:**
- **Day 0:** the B4 soup test and the B3 engine benchmark (no engineering).
- **Days 1–3:** A1, then A2.
- **Then:** B1 as S10 (with B3's throughput if it pans out).
- **After that:** B5, then A3 exploiters on the winner.
- A4 (Gumbel ExIt) only after A1/A2 show a reliable search gain, given kevinz917's failed distillation.

---

## 4. Attractive, but I'd advise against them here

- **LLM as the player or the strategist at play time** (language-conditioned plans from a local LLM driving the
  network).
  - Frontier-model bots won 0.3–3% against an RL bot in this game (Polytopia Bench).
  - CivRealm's LLM agents collapsed on tactics.
  - A local 14–20B model is weaker still and costs seconds per call.
  - PokéChamp's LLM-minimax reached a top-10–30% human level in Pokémon
    ([arXiv 2503.04094](https://arxiv.org/abs/2503.04094)), but against a far weaker baseline than a network at 98%
    vs Crazy.
  - Strategist (LLM bi-level tree search) was shown on GOPS and Avalon
    ([arXiv 2408.10635](https://arxiv.org/abs/2408.10635)), not a 4X game.
  - The non-LLM equivalent (choosing z by search, B5) keeps judgement learned.
- **LLM-designed or shaped rewards** (Eureka, Motif, potential shaping).
  - It is hand-written judgement by proxy.
  - Shaping hurt in this game: kevinz917's PBRS arm, 37% vs 40%.
  - It destabilized late in Generals.io.
  - Motif's NetHack success ([arXiv 2310.00166](https://arxiv.org/abs/2310.00166)) is about exploration in a
    sparse single-agent game, a different problem.
- **A learned world model (MuZero dynamics).**
  - The exact engine is fast (save/load 0.5 ms) and exact.
  - A learned model adds error and removes the project's main asset.
  - The only learned "model" worth having is the belief model (A7).
- **Decision transformers or return-conditioned imitation on the human set.** They fail in stochastic or
  adversarial settings because returns reflect luck (Paster et al. 2022,
  [arXiv 2205.15967](https://arxiv.org/abs/2205.15967)), and 7.7k games is small. Use advantage weighting (B6) or
  z-conditioning (B5) instead.
- **Deeper or wider search before the value is fixed** (depth ≥ 2, many replies, full MCTS over turns). It
  saturated at depth 1 / 1 reply in kevinz917's sweep, and the extra depth only inflated the chosen lane's apparent
  advantage.
- **Expert iteration that distills the argmax of noisy lanes.** kevinz917: no raw gain, and it lost about half the
  search gain. If ExIt, use Gumbel improved-policy targets (A4).
- **CFR, ReBeL, Student of Games, Obscuro-style subgame solving.**
  - Obscuro needed new theory to make fog-of-war chess tractable and is superhuman there (won 16 of 20 games against
    the top-rated human, scored 97% vs the prior state of the art)
    ([OpenReview](https://openreview.net/forum?id=afaakBqkvb)).
  - Polytopia's hidden information is mostly where things are and what they are, which determinization handles
    reasonably.
  - Revisit only if A3 finds exploits that hinge on hidden information.
- **Training on a re-implemented engine for speed** (kevinz917's Rust engine). It breaks the exact-rules rule, and it
  is Bardur-only. Use their weights only as an evaluation opponent (D1).
- **More PPO hyperparameter sweeps, small human-start sets, and the league as it stands.** All were falsified or
  neutral in S4–S9. The league needs a stronger exploiter (A3) before it can matter: AlphaStar's league gains came
  with exploiters that actually exploited.
- **A bigger model on its own as the next step.** kevinz917's 0.5M CNN beat Crazy 24–0 raw. The RL plateau here
  doesn't look capacity-bound; the critic, search and samples come first. B6 is for the next human pass, not
  instead of A1 or B1.

---

## 5. Sources consulted (main)

Polytopia-specific:
- kevinz917/superhuman-polytopia-bot: [repo](https://github.com/kevinz917/superhuman-polytopia-bot) and the docs
  linked above.
- Tribes: [AIIDE 2020](https://ojs.aaai.org/index.php/AIIDE/article/view/7438),
  [MCTS pruning](https://ceur-ws.org/Vol-2862/paper27.pdf),
  [Elastic MCTS](https://arxiv.org/abs/2205.15126).

Search under fog and hidden information:
- Ataraxos: [arXiv 2511.07312](https://arxiv.org/abs/2511.07312), [Nature](https://www.nature.com/articles/s41586-026-11036-y).
- Obscuro: [OpenReview](https://openreview.net/forum?id=afaakBqkvb).
- SPARTA: [arXiv 1912.02318](https://arxiv.org/abs/1912.02318).
- Learned Belief Search: [arXiv 2106.09086](https://arxiv.org/abs/2106.09086).
- Gumbel: [OpenReview](https://openreview.net/forum?id=bERaNdoegnO).
- Multiagent Gumbel MuZero: [AAAI 2024](https://ojs.aaai.org/index.php/AAAI/article/view/29121).
- OEP: [Justesen 2016](http://julian.togelius.com/Justesen2016Online.pdf).
- Evolutionary MCTS: [Baier & Cowling 2018](https://dl.acm.org/doi/10.1109/CIG.2018.8490403).

Self-play at scale:
- Generals.io superhuman: [arXiv 2606.23348](https://arxiv.org/abs/2606.23348); earlier
  [arXiv 2507.06825](https://arxiv.org/abs/2507.06825).
- KataGo: [arXiv 1902.10565](https://arxiv.org/abs/1902.10565).
- Jones: [arXiv 2104.03113](https://arxiv.org/abs/2104.03113).
- VRPO: [arXiv 2605.19235](https://arxiv.org/abs/2605.19235).
- Suphx: [arXiv 2003.13590](https://arxiv.org/abs/2003.13590).
- Gym-μRTS: [arXiv 2105.13807](https://arxiv.org/abs/2105.13807).

Exploitability:
- Adversarial Go: [arXiv 2211.00241](https://arxiv.org/abs/2211.00241).
- Approximate exploitability: [arXiv 2004.09677](https://arxiv.org/abs/2004.09677).

Human data:
- AlphaStar Unplugged: [arXiv 2308.03526](https://arxiv.org/abs/2308.03526).
- piKL: [arXiv 2112.07544](https://arxiv.org/abs/2112.07544). In chess and Go, KL-regularized search toward the
  imitation policy was stronger than imitation alone and predicted strong humans better.
- Diplodocus: [arXiv 2210.05492](https://arxiv.org/abs/2210.05492). Ranked 1st and 3rd in a 200-game tournament with
  62 humans.
- Maia-2: [arXiv 2409.20553](https://arxiv.org/abs/2409.20553).
- Grandmaster chess without search: [arXiv 2402.04494](https://arxiv.org/abs/2402.04494).
- Concept discovery: [arXiv 2310.16410](https://arxiv.org/abs/2310.16410).
- Paster et al. 2022: [arXiv 2205.15967](https://arxiv.org/abs/2205.15967).

LLMs:
- CivRealm: [arXiv 2401.10568](https://arxiv.org/abs/2401.10568).
- PokéChamp: [arXiv 2503.04094](https://arxiv.org/abs/2503.04094).
- Strategist: [arXiv 2408.10635](https://arxiv.org/abs/2408.10635).
- Motif: [arXiv 2310.00166](https://arxiv.org/abs/2310.00166).
- MiniRTS instructor/executor: [arXiv 1906.00744](https://arxiv.org/abs/1906.00744).
- TacticCraft: [arXiv 2507.15618](https://arxiv.org/abs/2507.15618).
- Polytopia Bench: [README](https://github.com/kevinz917/superhuman-polytopia-bot/blob/main/benchmark/README.md).
