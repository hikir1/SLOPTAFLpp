/*
   american fuzzy lop++ - fuzze_one routines in different flavours
   ---------------------------------------------------------------

   Originally written by Michal Zalewski

   Now maintained by Marc Heuse <mh@mh-sec.de>,
                        Heiko Eißfeldt <heiko.eissfeldt@hexco.de> and
                        Andrea Fioraldi <andreafioraldi@gmail.com>

   Copyright 2016, 2017 Google Inc. All rights reserved.
   Copyright 2019-2020 AFLplusplus Project. All rights reserved.

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at:

     http://www.apache.org/licenses/LICENSE-2.0

   This is the real deal: the program takes an instrumented binary and
   attempts a variety of basic fuzzing tricks, paying close attention to
   how they affect the execution path.

 */

#include "afl-fuzz.h"
#include <string.h>
#include <limits.h>
#include "cmplog.h"

#include <math.h>
#include <gsl/gsl_rng.h>
#include <gsl/gsl_randist.h>

static double kl(double p, double q) {
  return p*log(p/q) + (1-p)*log((1-p)/(1-q));
}

static double dkl(double p, double q) {
  return (q-p) / (q*(1-q));
}

static double klucb_klucb(klucb_t* inst, normal_bandit_arm* arm) {
  const double logndn = log(inst->time_step) / arm->num_selected;
  const double p = MAX(arm->sample_mean, KLUCB_DELTA);
  if (p >= 1) return 1;

  double q = p + KLUCB_DELTA;
  for (int t=0; t < 25; ++t) {
      const double f = logndn - kl(p, q);
      const double df = -dkl(p, q);
      if (f*f < KLUCB_EPS) break;

      q -= f/df;
      if (q < p + KLUCB_DELTA) q = p + KLUCB_DELTA;
      if (q > 1 - KLUCB_DELTA) q = 1 - KLUCB_DELTA;
  }

  return q;
}

double div_inf(double x, double y) {
    if (y == 0.0 || y == -0.0)
        return INFINITY;
    return x / y;
}

void exppp_gap_estimate(exppp_t *self, double *Delta) {
    double average_losses[EXP_MAX_N_ARMS];
    double exploration_term[EXP_MAX_N_ARMS];
    double UCB[EXP_MAX_N_ARMS];
    double LCB[EXP_MAX_N_ARMS];

    double min_UCB = INFINITY;
    for (u64 i = 0; i < self->n_arms; i++) {
        average_losses[i] = div_inf((double)self->unweighted_losses[i],
                (double)self->pulls[i]);
        exploration_term[i] = sqrt(div_inf(EXP_ALPHA * log(self->t) +
                    log(self->n_arms),  2 * self->pulls[i]));
        UCB[i] = MIN(1.0, average_losses[i] + exploration_term[i]);
        LCB[i] = MAX(0.0, average_losses[i] - exploration_term[i]);
        min_UCB = MIN(UCB[i], min_UCB);
    }
    for (u64 i = 0; i < self->n_arms; i++) {
        Delta[i] = MAX(0.0, LCB[i] - min_UCB);
        // if (self->t >= self->n_arms) {
        // //printf("D[%d]: %lf\n", i, Delta[i]);
        // //printf("LCB[%d]: %lf\n", i, LCB[i]);
        // //printf("average_losses[%d]: %lf\n", i, average_losses[i]);
        // //printf("exploration_term[%d]: %lf\n", i, exploration_term[i]);
        //     assert(Delta[i] >= 0.0);
        //     assert(Delta[i] <= 1.0);
        // }
    }
}

double exppp_xi(exppp_t *self, u64 arm, double *gap_estimated) {
    return div_inf(EXP_BETA * log(self->t), (self->t * (pow(gap_estimated[arm],
                        2))));
}

void exppp_epsilon(exppp_t *self, double epsilons[EXP_MAX_N_ARMS]) {
    double gap_estimated[EXP_MAX_N_ARMS];
    exppp_gap_estimate(self, gap_estimated);
    for (u64 arm = 0; arm < self->n_arms; arm++) {
        //printf("gap_estimated[%d]=%lf\n", arm, gap_estimated[arm]);
        //printf("%d %lf %lf\n", self->t, pow(gap_estimated[arm], 2), exppp_xi(self, arm, gap_estimated));

        epsilons[arm] = MIN(MIN(0.5 / self->n_arms, 0.5 * sqrt(log(self->n_arms) /
                        self->t/ self->n_arms)), exppp_xi(self, arm, gap_estimated));
    }
}

double exppp_eta(exppp_t *self) {
  return 0.5 * sqrt(log(self->n_arms) / (double)self->n_arms/ (double)(self->t+1));
}

void exppp_update_trusts(exppp_t *self) {
  // double eta = exppp_eta(self);
  // assert(0.0 <= eta && eta <= 1.0);
  double sum_of_trusts = 0.0;

  double epsilons[EXP_MAX_N_ARMS] = {};
  exppp_epsilon(self, epsilons);
  double sum_of_epsilons = 0.0;
  for (u64 i = 0; i < self->n_arms; i++) {
      sum_of_epsilons += epsilons[i];
  }

  for (u64 i = 0; i < self->n_arms; i++) {
    self->trusts[i] = ((1.0 - sum_of_epsilons) * self->weights[i]) +
        epsilons[i];

    sum_of_trusts += self->trusts[i];
  }
  // numpy's default tolerance
  if (sum_of_trusts < 1e-08) {
    for (u64 i = 0; i < self->n_arms; i++) {
      self->trusts[i] = 1.0 / self->n_arms;
    }
    sum_of_trusts = 1.0;
  }
  for (u64 i = 0; i < self->n_arms; i++){
    self->trusts[i] /= sum_of_trusts;
  }
}

void exppp_add_reward(exppp_t* self, int arm, double reward) {
  // assert(0.0 <= reward && reward <= 1.0);
  self->total_rewards[arm] += (int)reward;
  reward = (reward - EXP_LOWER) / EXP_AMPLITUDE;
  double loss = 1.0 - reward;
  self->unweighted_losses[arm] += loss;

  loss = loss / self->trusts[arm];
  self->losses[arm] += loss;

  double sum_of_weights = 0.0;
  double eta = exppp_eta(self);
  double min_loss_eta = INFINITY;
  for (u64 i = 0; i < self->n_arms; i++) {
      min_loss_eta = MIN(min_loss_eta, -eta * self->losses[i]);
  }
  for (u64 i=0; i < self->n_arms; i++) {
    self->weights[i] = exp(- eta * self->losses[i] -min_loss_eta);
    sum_of_weights += self->weights[i];
  }
  for (u64 i=0; i < self->n_arms; i++) {
    self->weights[i] /= sum_of_weights;
  }
}

u64 choice_from_distribution(afl_state_t *afl, exppp_t *self) {
  double sum_of_possibility = 0.0;
  double target = gsl_rng_uniform(afl->gsl_rng_state);

  exppp_update_trusts(self);

  for (u64 i = 0; i < self->n_arms; i++) {
    sum_of_possibility += self->trusts[i];
    if (target < sum_of_possibility) {
      return i;
    }
  }
  return self->n_arms - 1;
}

u64 exppp_select_arm(afl_state_t *afl, exppp_t* self, u8* mask) {
  u64 choice;
  self->t++;
  if (self->t <= self->n_arms) {
    choice = self->t - 1;
  } else {
    choice = choice_from_distribution(afl, self);
  }
  self->pulls[choice] += 1;
  return choice;
}

u64 expix_select_arm(afl_state_t *afl, expix_t* self, u8* mask) {
  self->t++;

  double sum_of_possibility = 0.0;
  double target = gsl_rng_uniform(afl->gsl_rng_state);

  for (u64 i = 0; i < self->n_arms; i++) {
    sum_of_possibility += self->weights[i];
    if (target < sum_of_possibility) {
      self->pulls[i]++;
      return i;
    }
  }
  self->pulls[self->n_arms - 1]++;
  return self->n_arms - 1;
}

void expix_add_reward(expix_t* self, int arm, double reward) {
  self->total_rewards[arm] += (int)reward;

  double eta = sqrt(2 * log(self->n_arms) / self->n_arms / self->t);
  double gamma = eta/2;

  double loss = 1.0 - reward;
  loss = loss / (self->weights[arm] + gamma);
  self->losses[arm] += loss;

  double min_loss = INFINITY;
  for (u64 i = 0; i < self->n_arms; i++) {
      min_loss = MIN(min_loss, self->losses[i]);
  }

  double denom = 0;
  for (u64 i = 0; i < self->n_arms; i++) {
      self->weights[i] = exp(-eta * (self->losses[i] - min_loss));
      denom += self->weights[i];
  }

  for (u64 i=0; i < self->n_arms; i++) {
    self->weights[i] /= denom;
  }
}

/* Adwin */

void init_adwin(adwin_t *ret) {
  ret->head = calloc(1, sizeof(adwin_node_t));
  ret->tail = ret->head;
}

void dest_adwin(adwin_t* adwin) {
  adwin_node_t* node;
  for (node=adwin->head; node; ) {
    adwin_node_t* nxt = node->next;
    free(node);
    node = nxt;
  }
}

void adwin_remove_front_windows(adwin_node_t* node, int num) {
  int i;
  int lim = node->size - num;
  for (i=0; i<lim; i++) {
    node->sum[i] = node->sum[i+num];
  }
  node->size -= num;
}

void adwin_add_tail_window(adwin_node_t* node, u64 s) {
  node->sum[node->size++] = s;
}

adwin_node_t* adwin_add_tail_node(adwin_t* adwin) {
  adwin->last_node_idx++;

  adwin_node_t* new_tail = calloc(1, sizeof(adwin_node_t));
  
  adwin->tail->next = new_tail;
  new_tail->prev = adwin->tail;
  
  adwin->tail = new_tail;

  return adwin->tail;
}

void adwin_expire_last_window(adwin_t* adwin) {
  adwin->W -= 1ull << adwin->last_node_idx;
  adwin->sum -= adwin->tail->sum[0];

  adwin_remove_front_windows(adwin->tail, 1);

  if (adwin->tail->size == 0 && adwin->tail != adwin->head) {
    adwin_node_t* new_tail = adwin->tail->prev;
    free(adwin->tail);

    new_tail->next = NULL;
    adwin->tail = new_tail;

    adwin->last_node_idx--;
  }
}

void adwin_normlize_buckets(adwin_t* adwin) {
  int exp;
  adwin_node_t* node;

  for (exp=0, node=adwin->head; node; exp++, node=node->next) {
    if (node->size <= ADWIN_M) break;

    adwin_node_t* next = node->next;
    if (!next) {
      next = adwin_add_tail_node(adwin);
    }
    
    // The calculation of variation in the original adwin implementation seems wrong 
    u64 s = node->sum[0] + node->sum[1];
    adwin_add_tail_window(node->next, s);

    adwin_remove_front_windows(node, 2);
  }
}

inline u8 adwin_should_drop(u64 s0, u64 n0, u64 s1, u64 n1, double ddv2, double dd2_3) {
  double u0 = s0 / (double)n0;
  double u1 = s1 / (double)n1;
  double du = u0 - u1;

  double inv_m = 1.0 / (1 + n0 - ADWIN_MIN_ELEM_TO_CHECK) + 1.0 / (1 + n1 - ADWIN_MIN_ELEM_TO_CHECK);
  double eps = sqrt(ddv2*inv_m) + dd2_3 * inv_m;
  
  if (fabs(du) > eps) return 1;
  
  return 0;
}

void adwin_drop_last_till_identical(adwin_t* adwin) {
  if (adwin->W < ADWIN_MIN_ELEM_TO_START_DROP) return;

  while (1) {
    bool dropped = false;

    u64 n0 = 0;
    u64 s0 = 0;
    u64 n1 = adwin->W;
    u64 s1 = adwin->sum;
    int exp = adwin->last_node_idx;

    double n = adwin->W;
    double dd2 = log(2.0 * log(n) / ADWIN_DELTA) * 2;
    double u = adwin->sum / n;
    double ddv2 = u * (1-u) * dd2;
    double dd2_3 = dd2 / 3.0;

    adwin_node_t* node;
    for (node=adwin->tail; node; node=node->prev) {
      int k;
      for (k=0; k < node->size; k++) {
        n0 += 1ull << exp;
        n1 -= 1ull << exp;
        s0 += node->sum[k];
        s1 -= node->sum[k];

        if (n1 < ADWIN_MIN_ELEM_TO_CHECK) goto L_CHECK_END;
        if (n0 < ADWIN_MIN_ELEM_TO_CHECK) continue;

        if (adwin_should_drop(s0, n0, s1, n1, ddv2, dd2_3)) {

#ifdef ADWIN_ADAPTIVE_RESETTING
          dest_adwin(adwin);
          memset(adwin, 0, sizeof(adwin_t));
          init_adwin(adwin);
#else
          dropped = true;
          adwin_expire_last_window(adwin);
#endif
          goto L_CHECK_END;
        }
      }
      --exp;
    }

L_CHECK_END:

    if (!dropped) break;
  }
}

void adwin_add_elem(adwin_t* adwin, u8 reward) {
  adwin->W++;
  adwin->sum += reward;
  adwin_add_tail_window(adwin->head, reward);

  adwin_normlize_buckets(adwin);

#if ADWIN_DROP_INTERVAL != 1
  adwin->num_add++;
  if (adwin->num_add != ADWIN_DROP_INTERVAL) return;
  adwin->num_add = 0;
#endif

  adwin_drop_last_till_identical(adwin);
}

double adwin_get_estimation(adwin_t* adwin) {
  if (adwin->W > 0) return adwin->sum / (double)adwin->W;
  return 0;
}

inline void uniform_add_reward(uniform_t* inst, int idx, u8 r) {
  uniform_bandit_arm *arm = &inst->arms[idx];
  arm->num_selected++;
  arm->total_rewards += r;
}

inline void ucb_add_reward(ucb_t* inst, int idx, u8 r) {
  inst->time_step++;

  normal_bandit_arm *arm = &inst->arms[idx];

  arm->num_selected++;
  arm->total_rewards += r;
  arm->sample_mean = ((double)(arm->total_rewards))/(arm->num_selected);
}

inline void klucb_add_reward(klucb_t* inst, int idx, u8 r) {
  inst->time_step++;

  normal_bandit_arm *arm = &inst->arms[idx];

  arm->num_selected++;
  arm->total_rewards += r;
  arm->sample_mean = ((double)(arm->total_rewards))/(arm->num_selected);
}

inline void ts_add_reward(ts_t* inst, int idx, u8 r) {
  normal_bandit_arm *arm = &inst->arms[idx];

  arm->num_selected++;
  arm->total_rewards += r;
  arm->sample_mean = ((double)(arm->total_rewards))/(arm->num_selected);
}

inline void adsts_add_reward(adsts_t* inst, int idx, u8 r) {
  adwin_bandit_arm *arm = &inst->arms[idx];

  arm->num_selected++;
  arm->total_rewards += r;
  adwin_add_elem(&arm->adwin, r);
}

inline void dts_add_reward(dts_t* inst, int idx, u8 r) {
  dts_bandit_arm *arm = &inst->arms[idx];

  arm->num_selected++;
  arm->num_rewarded += r;

  // already discounted in dts_select_arm
  arm->total_rewards += r;
  arm->total_losses  += 1 - r;
}

inline void dbe_add_reward(dbe_t* inst, int idx, u8 r) {
  dbe_bandit_arm *arm = &inst->arms[idx];

  arm->num_selected++;
  arm->num_rewarded += r;

  // already discounted in dbe_select_arm
  arm->total_rewards += r;
  arm->dis_num_selected += 1;
  // Note that, sample_mean of the other arms that are not selected, will remain the same
  // since total_rewards' = total_rewards * gamma and dis_num_selected' = dis_num_selected * gamma,
  // so total_rewards' / dis_num_selected' = total_rewards / dis_num_selected
  arm->sample_mean = arm->total_rewards / arm->dis_num_selected;
}

inline u64 normal_num_selected(normal_bandit_arm* arm) {
  return arm->num_selected;
}

inline u64 normal_total_rewards(normal_bandit_arm* arm) {
  return arm->total_rewards;
}

inline double normal_sample_mean(normal_bandit_arm* arm) {
  return arm->sample_mean;
}

inline u64 adwin_num_selected(adwin_bandit_arm* arm) {
  return arm->adwin.W;
}

inline u64 adwin_total_rewards(adwin_bandit_arm* arm) {
  return arm->adwin.sum;
}

inline double adwin_sample_mean(adwin_bandit_arm* arm) {
  return adwin_get_estimation(&arm->adwin);
}

/* Bandit */

int dts_select_arm(afl_state_t *afl, dts_t* inst, u8* mask) {
  int i;
  double max_sampled = -1;
  int selected_idx = 0;
  int n = inst->n_arms;
  dts_bandit_arm *slots = inst->arms;

  for (i = 0; i < n; i++) {
    if (mask && mask[i]) continue;

    double a = slots[i].total_rewards + 1;
    double b = slots[i].total_losses  + 1;
    double sampled = gsl_ran_beta(afl->gsl_rng_state, a, b);

#ifdef OPTIMISTIC_DTS
    // dOTS
    double beta_mean = a/(a+b); 
    if (sampled < beta_mean) sampled = beta_mean;
#endif

    if (sampled > max_sampled) {
      max_sampled = sampled;
      selected_idx = i;
    }
  }

  for (i = 0; i < n; i++) {
    // We need to discount rewards even if skipping the arm.
    slots[i].total_rewards *= DTS_GAMMA;
    slots[i].total_losses  *= DTS_GAMMA;
  }

  return selected_idx;
}

int dbe_select_arm(afl_state_t *afl, dbe_t *inst, u8* mask) {
  // We don't prepare SIVO's constants such as
  // SAMPLE_RANDOMLY_UNTIL_ROUND, SAMPLE_RANDOMLY_THIS_ROUND, 
  // SAMPLE_NOT_RANDOMLY, SAMPLE_RANDOMLY_THIS_ROUND_NO_UPDATE.
  // the last 3 constants don't make sense because 
  // bandit algorithms are designed to minimize regret from the beggining, 
  // and ignoring it and using uniform distribution sometimes destroys
  // its performance and theoretical guarantees.
  // On the other hand, SAMPLE_RANDOMLY_UNTIL_ROUND may be legitimate, 
  // since it can be considered as preparing more accurate prior distributions 
  // than uniform distributions, and since that preprocess is commonly used 
  // also in other algorithms like eps-greedy.
  // However, recalling that this is a non-stationary setting, 
  // and that initial samples will be forgot at some time, 
  // the preprocess of drawing values from uniform distributions to obtain initial estimates 
  // of expected rewards is not so meaningful.

  int index = 0;
  int n = inst->n_arms;
  dbe_bandit_arm *slots = inst->arms;

  double max_avg = 0;
  double redcoef = 1.0;
  int ACTIVE = 0;

  for (int i=0; i<n; i++) {
    if (mask && mask[i]) continue;

    ACTIVE++;
    // Lazily update sample_mean
    if (slots[i].dis_num_selected > 0) {
      if (max_avg < slots[i].sample_mean) max_avg = slots[i].sample_mean;
    }
  }

  // i'm not sure what this heuristics means :(
  if (max_avg > 0) redcoef = 1.0 / (2.0 * max_avg);

  // maybe this is working as a kind of adaptive resetting bandit(?)
  // if so, this is not a pure discounting algorithm...
  if (redcoef > 1 << 30) {
    for (int i=0; i<n; i++) {
      slots[i].total_rewards = 1.0;
      slots[i].dis_num_selected = 1.0;
      slots[i].sample_mean = 1.0;
    }
  }

  int *indices = malloc(n * sizeof(int));
  int num_indices = 0;
  // pick index if not sampled 
  for (int i=0; i<n; i++) {
    if (mask && mask[i]) continue;
    if (slots[i].dis_num_selected <= 0) {
      indices[num_indices++] = i;
    }
  }
  if (num_indices > 0) {
    // probably just returning 0 is the same...(it's a negligible difference)
    return indices[rand_below(afl, num_indices)];
  }

  double *w = calloc(n, sizeof(double));
  double cur, beta;
  for (int i=0; i<n; i++) {
    if (mask && mask[i]) continue; // due to calloc, w[i] = 0, so no problem

    beta = 4 + 2 * ACTIVE;

    // Our bandit problems are not small bandit problem like 2 arms
    /*if( x[i].allow_small > 0 )
      beta = x[i].allow_small;*/
 
    cur = beta * (redcoef * slots[i].sample_mean);
    // Follow SIVO. Though I'm not sure about other heuristics,
    // this is valid since 2^x = e^(x*log_e(2))
    w[i] = pow(2, cur);
  }

  gsl_ran_discrete_t *lookup = gsl_ran_discrete_preproc(n, w);
  index = gsl_ran_discrete(afl->gsl_rng_state, lookup);
  gsl_ran_discrete_free(lookup);

  for (int i = 0; i < n; i++) {
    // We need to discount rewards even if skipping the arm.
    slots[i].total_rewards *= DBE_GAMMA;
    slots[i].dis_num_selected  *= DBE_GAMMA;
  }

  return index;
}

int uniform_select_arm(afl_state_t *afl, ucb_t *inst, u8* mask) {
  int i;
  int n = inst->n_arms;

  int cnt = 0;
  for (i = 0; i < n; i++) {
    if (mask && mask[i]) continue;
    cnt++;
  }

  int k = rand_below(afl, cnt);
  for (i = 0; i < n; i++) {
    if (mask && mask[i]) continue;
    if (!k) return i;
    --k;
  }

  assert(0);
}

int ucb_select_arm(afl_state_t *afl, ucb_t *inst, u8* mask) {
  int i;
  double max_ucb = -1;
  int selected_idx;
  int n = inst->n_arms;
  normal_bandit_arm *slots = inst->arms;

  for (i = 0; i < n; i++) {
    if (mask && mask[i]) continue;

    if (normal_num_selected(&slots[i]) == 0) {
      selected_idx = i;
      break;
    }

    double ucb = normal_sample_mean(&slots[i])
              + sqrt(2 * log(inst->time_step) / normal_num_selected(&slots[i]));
    if (ucb > max_ucb) {
      max_ucb = ucb;
      selected_idx = i;
    }
  }

  return selected_idx;
}

int klucb_select_arm(afl_state_t *afl, klucb_t *inst, u8* mask) {
  int i;
  double max_ucb = -1;
  int selected_idx;
  int n = inst->n_arms;
  normal_bandit_arm *slots = inst->arms;

  for (i = 0; i < n; i++) {
    if (mask && mask[i]) continue;

    if (normal_num_selected(&slots[i]) == 0) {
      selected_idx = i;
      break;
    }


    double ucb = klucb_klucb(inst, &slots[i]);
    if (ucb > max_ucb) {
      max_ucb = ucb;
      selected_idx = i;
    }
  }

  return selected_idx;
}

int ts_select_arm(afl_state_t *afl, ts_t* inst, u8* mask) {
  int i;
  int n = inst->n_arms;
  normal_bandit_arm *slots = inst->arms;

  double max_sampled = -1;
  int selected_idx = 0;

  for (i = 0; i < n; i++) {
    if (mask && mask[i]) continue;

    u64 total_rewards = normal_total_rewards(&slots[i]);
    u64 a = total_rewards + 1;
    u64 b = normal_num_selected(&slots[i]) - total_rewards + 1;
    double sampled = gsl_ran_beta(afl->gsl_rng_state, (double)a, (double)b);
    if (sampled > max_sampled) {
      max_sampled = sampled;
      selected_idx = i;
    }
  }

  return selected_idx;
}

int adsts_select_arm(afl_state_t *afl, adsts_t* inst, u8* mask) {
  int i;
  int n = inst->n_arms;
  adwin_bandit_arm *slots = inst->arms;

  double max_sampled = -1;
  int selected_idx = 0;

  for (i = 0; i < n; i++) {
    if (mask && mask[i]) continue;

    u64 total_rewards = adwin_total_rewards(&slots[i]);
    u64 a = total_rewards + 1;
    u64 b = adwin_num_selected(&slots[i]) - total_rewards + 1;
    double sampled = gsl_ran_beta(afl->gsl_rng_state, (double)a, (double)b);
    if (sampled > max_sampled) {
      max_sampled = sampled;
      selected_idx = i;
    }
  }

  return selected_idx;
}

/* MOpt */

static int select_algorithm(afl_state_t *afl, u32 max_algorithm) {

  int i_puppet, j_puppet = 0, operator_number = max_algorithm;

  double range_sele =
      (double)afl->probability_now[afl->swarm_now][operator_number - 1];
  double sele = ((double)(rand_below(afl, 10000) * 0.0001 * range_sele));

  for (i_puppet = 0; i_puppet < operator_num; ++i_puppet) {

    if (unlikely(i_puppet == 0)) {

      if (sele < afl->probability_now[afl->swarm_now][i_puppet]) { break; }

    } else {

      if (sele < afl->probability_now[afl->swarm_now][i_puppet]) {

        j_puppet = 1;
        break;

      }

    }

  }

  if ((j_puppet == 1 &&
       sele < afl->probability_now[afl->swarm_now][i_puppet - 1]) ||
      (i_puppet + 1 < operator_num &&
       sele > afl->probability_now[afl->swarm_now][i_puppet + 1])) {

    FATAL("error select_algorithm");

  }

  return i_puppet;

}

/* Helper to choose random block len for block operations in fuzz_one().
   Doesn't return zero, provided that max_len is > 0. */

static inline u32 choose_block_len(afl_state_t *afl, u32 limit) {

  u32 min_value, max_value;
  u32 rlim = MIN(afl->queue_cycle, (u32)3);

  if (unlikely(!afl->run_over10m)) { rlim = 1; }

  switch (rand_below(afl, rlim)) {

    case 0:
      min_value = 1;
      max_value = HAVOC_BLK_SMALL;
      break;

    case 1:
      min_value = HAVOC_BLK_SMALL;
      max_value = HAVOC_BLK_MEDIUM;
      break;

    default:

      if (likely(rand_below(afl, 10))) {

        min_value = HAVOC_BLK_MEDIUM;
        max_value = HAVOC_BLK_LARGE;

      } else {

        min_value = HAVOC_BLK_LARGE;
        max_value = HAVOC_BLK_XL;

      }

  }

  if (min_value >= limit) { min_value = 1; }

  return min_value + rand_below(afl, MIN(max_value, limit) - min_value + 1);

}

/* Helper function to see if a particular change (xor_val = old ^ new) could
   be a product of deterministic bit flips with the lengths and stepovers
   attempted by afl-fuzz. This is used to avoid dupes in some of the
   deterministic fuzzing operations that follow bit flips. We also
   return 1 if xor_val is zero, which implies that the old and attempted new
   values are identical and the exec would be a waste of time. */

static u8 could_be_bitflip(u32 xor_val) {

  u32 sh = 0;

  if (!xor_val) { return 1; }

  /* Shift left until first bit set. */

  while (!(xor_val & 1)) {

    ++sh;
    xor_val >>= 1;

  }

  /* 1-, 2-, and 4-bit patterns are OK anywhere. */

  if (xor_val == 1 || xor_val == 3 || xor_val == 15) { return 1; }

  /* 8-, 16-, and 32-bit patterns are OK only if shift factor is
     divisible by 8, since that's the stepover for these ops. */

  if (sh & 7) { return 0; }

  if (xor_val == 0xff || xor_val == 0xffff || xor_val == 0xffffffff) {

    return 1;

  }

  return 0;

}

/* Helper function to see if a particular value is reachable through
   arithmetic operations. Used for similar purposes. */

static u8 could_be_arith(u32 old_val, u32 new_val, u8 blen) {

  u32 i, ov = 0, nv = 0, diffs = 0;

  if (old_val == new_val) { return 1; }

  /* See if one-byte adjustments to any byte could produce this result. */

  for (i = 0; (u8)i < blen; ++i) {

    u8 a = old_val >> (8 * i), b = new_val >> (8 * i);

    if (a != b) {

      ++diffs;
      ov = a;
      nv = b;

    }

  }

  /* If only one byte differs and the values are within range, return 1. */

  if (diffs == 1) {

    if ((u8)(ov - nv) <= ARITH_MAX || (u8)(nv - ov) <= ARITH_MAX) { return 1; }

  }

  if (blen == 1) { return 0; }

  /* See if two-byte adjustments to any byte would produce this result. */

  diffs = 0;

  for (i = 0; (u8)i < blen / 2; ++i) {

    u16 a = old_val >> (16 * i), b = new_val >> (16 * i);

    if (a != b) {

      ++diffs;
      ov = a;
      nv = b;

    }

  }

  /* If only one word differs and the values are within range, return 1. */

  if (diffs == 1) {

    if ((u16)(ov - nv) <= ARITH_MAX || (u16)(nv - ov) <= ARITH_MAX) {

      return 1;

    }

    ov = SWAP16(ov);
    nv = SWAP16(nv);

    if ((u16)(ov - nv) <= ARITH_MAX || (u16)(nv - ov) <= ARITH_MAX) {

      return 1;

    }

  }

  /* Finally, let's do the same thing for dwords. */

  if (blen == 4) {

    if ((u32)(old_val - new_val) <= ARITH_MAX ||
        (u32)(new_val - old_val) <= ARITH_MAX) {

      return 1;

    }

    new_val = SWAP32(new_val);
    old_val = SWAP32(old_val);

    if ((u32)(old_val - new_val) <= ARITH_MAX ||
        (u32)(new_val - old_val) <= ARITH_MAX) {

      return 1;

    }

  }

  return 0;

}

/* Last but not least, a similar helper to see if insertion of an
   interesting integer is redundant given the insertions done for
   shorter blen. The last param (check_le) is set if the caller
   already executed LE insertion for current blen and wants to see
   if BE variant passed in new_val is unique. */

static u8 could_be_interest(u32 old_val, u32 new_val, u8 blen, u8 check_le) {

  u32 i, j;

  if (old_val == new_val) { return 1; }

  /* See if one-byte insertions from interesting_8 over old_val could
     produce new_val. */

  for (i = 0; i < blen; ++i) {

    for (j = 0; j < sizeof(interesting_8); ++j) {

      u32 tval =
          (old_val & ~(0xff << (i * 8))) | (((u8)interesting_8[j]) << (i * 8));

      if (new_val == tval) { return 1; }

    }

  }

  /* Bail out unless we're also asked to examine two-byte LE insertions
     as a preparation for BE attempts. */

  if (blen == 2 && !check_le) { return 0; }

  /* See if two-byte insertions over old_val could give us new_val. */

  for (i = 0; (u8)i < blen - 1; ++i) {

    for (j = 0; j < sizeof(interesting_16) / 2; ++j) {

      u32 tval = (old_val & ~(0xffff << (i * 8))) |
                 (((u16)interesting_16[j]) << (i * 8));

      if (new_val == tval) { return 1; }

      /* Continue here only if blen > 2. */

      if (blen > 2) {

        tval = (old_val & ~(0xffff << (i * 8))) |
               (SWAP16(interesting_16[j]) << (i * 8));

        if (new_val == tval) { return 1; }

      }

    }

  }

  if (blen == 4 && check_le) {

    /* See if four-byte insertions could produce the same result
       (LE only). */

    for (j = 0; j < sizeof(interesting_32) / 4; ++j) {

      if (new_val == (u32)interesting_32[j]) { return 1; }

    }

  }

  return 0;

}

#ifndef IGNORE_FINDS

/* Helper function to compare buffers; returns first and last differing offset.
   We use this to find reasonable locations for splicing two files. */

static void locate_diffs(u8 *ptr1, u8 *ptr2, u32 len, s32 *first, s32 *last) {

  s32 f_loc = -1;
  s32 l_loc = -1;
  u32 pos;

  for (pos = 0; pos < len; ++pos) {

    if (*(ptr1++) != *(ptr2++)) {

      if (f_loc == -1) { f_loc = pos; }
      l_loc = pos;

    }

  }

  *first = f_loc;
  *last = l_loc;

  return;

}

#endif                                                     /* !IGNORE_FINDS */

/* True if branch_ids contains branch_id*/
static int contains_id(int branch_id, int* branch_ids){
  for (int i = 0; branch_ids[i] != -1; i++){
    if (branch_ids[i] == branch_id) return 1;
	}
  return 0; 
}

/* you'll have to free the return pointer. */
static int* get_lowest_hit_branch_ids(afl_state_t *afl){
  int * rare_branch_ids = ck_alloc(sizeof(int) * afl->max_rare_branches);
  int lowest_hob = INT_MAX;
  u32 ret_list_size = 0;

  for (u32 i = 0; (i < afl->fsrv.map_size) && (ret_list_size < afl->max_rare_branches - 1); i++){
    // ignore unseen branches. sparse array -> unlikely 
    if (unlikely(afl->hit_bits[i] > 0)){
      if (contains_id(i, afl->blacklist)) continue;
      unsigned int long cur_hits = afl->hit_bits[i];
      int highest_order_bit = 0;
      while(cur_hits >>=1)
          highest_order_bit++;
      lowest_hob = highest_order_bit < lowest_hob ? highest_order_bit : lowest_hob;
      if (highest_order_bit < afl->rare_branch_exp){
        // if we are an order of magnitude smaller, prioritize the
        // rarer branches
        if (highest_order_bit < afl->rare_branch_exp - 1){
          afl->rare_branch_exp = highest_order_bit + 1;
          // everything else that came before had way more hits
          // than this one, so remove from list
          ret_list_size = 0;
        }
        rare_branch_ids[ret_list_size] = i;
        ret_list_size++;
      }

    }
  }

  if (ret_list_size == 0){
    DEBUG1("Was returning list of size 0\n");
    if (lowest_hob != INT_MAX) {
      afl->rare_branch_exp = lowest_hob + 1;
      DEBUG1("Upped max exp to %i\n", afl->rare_branch_exp);
      ck_free(rare_branch_ids);
      return get_lowest_hit_branch_ids(afl);
    }
  }

  rare_branch_ids[ret_list_size] = -1;
  return rare_branch_ids;

}

// checks if hits a rare branch with mini trace bits
// returns NULL if the trace bits does not hit a rare branch
// else returns a list of all the rare branches hit
// by the mini trace bits, in decreasing order of rarity
static u32 * is_rb_hit_mini(afl_state_t *afl, u8* trace_bits_mini){
  int * rarest_branches = get_lowest_hit_branch_ids(afl);
  u32 * branch_ids = ck_alloc(sizeof(u32) * afl->max_rare_branches);
  u32 * branch_cts = ck_alloc(sizeof(u32) * afl->max_rare_branches);
  int min_hit_index = 0;

  for (u32 i = 0; i < afl->fsrv.map_size ; i ++){
;
      if (unlikely (trace_bits_mini[i >> 3]  & (1 <<(i & 7)) )){
        int cur_index = i;
        int is_rare = contains_id(cur_index, rarest_branches);
        if (is_rare) {
          // at loop initialization, set min_branch_hit properly
          if (!min_hit_index) {
            branch_cts[min_hit_index] = afl->hit_bits[cur_index];
            branch_ids[min_hit_index] = cur_index + 1;
          }
          // in general just check if we're a smaller branch 
          // than the previously found min
          int j;
          for (j = 0 ; j < min_hit_index; j++){
            if (afl->hit_bits[cur_index] <= branch_cts[j]){
              memmove(branch_cts + j + 1, branch_cts + j, min_hit_index -j);
              memmove(branch_ids + j + 1, branch_ids + j, min_hit_index -j);
              branch_cts[j] = afl->hit_bits[cur_index];
              branch_ids[j] = cur_index + 1;
            }
          }
          // append at end
          if (j == min_hit_index){
            branch_cts[j] = afl->hit_bits[cur_index];
            // + 1 so we can distinguish 0 from other cases
            branch_ids[j] = cur_index + 1;

          }
          // this is only incremented when is_rare holds, which should
          // only happen a max of MAX_RARE_BRANCHES -1 times -- the last
          // time we will never reenter so this is always < MAX_RARE_BRANCHES
          // at the top of the if statement
          min_hit_index++;
        }
      }

  }
  ck_free(branch_cts);
  ck_free(rarest_branches);
  if (min_hit_index == 0){
      ck_free(branch_ids);
      branch_ids = NULL;
  } else {
    // 0 terminate the array
    branch_ids[min_hit_index] = 0;
  }
  return branch_ids;

}


/* Trim for a particular branch. Possibly modified contents of
   in_bur, and returns the new in_len. */


static u32 trim_case_rb(afl_state_t * afl, u8* in_buf, u32 in_len, u8* out_buf) {

  DEBUG1 ("entering RB trim, len is %i\n", in_len);

  if (afl->rb_fuzzing == 0){
    // @RB@ this should not happen. 
    return in_len;
  }

  static u8 tmp[64];

  u8  fault = 0;
  u32 trim_exec = 0;
  u32 remove_len;
  u32 len_p2;

  /* Although the trimmer will be less useful when variable behavior is
     detected, it will still work to some extent, so we don't check for
     this. */

  if (in_len < 5) return 0;

  afl->stage_name = tmp;
  afl->stage_short= "rbtrim";
  // CAROTODO: what is this, update later
  //bytes_trim_in += in_len;

  /* Select initial chunk len, starting with large steps. */

  len_p2 = next_p2(in_len);

  // CAROTODO: could make TRIM_START_STEPS smaller   
  remove_len = MAX(len_p2 / TRIM_START_STEPS, (u32) TRIM_MIN_BYTES);

  /* Continue until the number of steps gets too high or the stepover
     gets too small. */

  while (remove_len >= MAX(len_p2 / TRIM_END_STEPS, (u32) TRIM_MIN_BYTES)) {

    // why doesn't this start at 0?
    // u32 remove_pos = remove_len;
    u32 remove_pos = 0;
    char int_buf[STRINGIFY_VAL_SIZE_MAX] = {};
    u_stringify_int(int_buf, remove_len);

    sprintf(tmp, "rb trim %s/%s", int_buf, int_buf);

    afl->stage_cur = 0;
    afl->stage_max = in_len / remove_len;

    while (remove_pos < in_len) {

      u32 trim_avail = MIN(remove_len, in_len - remove_pos);

      //write_with_gap(in_buf, q->len, remove_pos, trim_avail);
      // HEAD
      memcpy(out_buf, in_buf, remove_pos);
      // TAIL
      memcpy(out_buf + remove_pos, in_buf + remove_pos + trim_avail, in_len - remove_pos - trim_avail);

      // not actually fault...
      /* using common fuzz stuff prevents us from having to mess with
         permanent changes to the queue */
      fault = common_fuzz_stuff(afl, out_buf, in_len - trim_avail);
   
      // Not sure if we want this given that fault is no longer a fault
      if (afl->stop_soon || fault) goto abort_rb_trimming;

      // if successfully hit branch of interest...
      if (hits_branch(afl, afl->rb_fuzzing - 1)) {
        // (0) calclength of tail
        u32 move_tail = in_len - remove_pos - trim_avail;
        // (1) reduce length by how much was trimmed
        in_len -= trim_avail;

        // (2) update the closest power of 2 len
        len_p2  = next_p2(in_len);
        memmove(in_buf + remove_pos, in_buf + remove_pos + trim_avail, 
                move_tail);
          
      } else remove_pos += remove_len;


      if (!(trim_exec++ % afl->stats_update_freq)) show_stats(afl);
      afl->stage_cur++;
      /* Note that we don't keep track of crashes or hangs here; maybe TODO? */
      }


    remove_len >>= 1;
    }
  
abort_rb_trimming:
  //@RM@ TODO: update later
 // bytes_trim_out += in_len;
  DEBUG1 (afl, "output of rb trimming has len %i\n", in_len);
  return in_len;

}

/* create a new branch mask of the specified size */

static inline u8* alloc_branch_mask(u32 size) {

  u8* mem;

  if (!size) return NULL;
  mem = ck_alloc_nozero(size);

  memset(mem, 7, size);

  mem[size - 1] = 4;

  return mem;

}

/* get a random modifiable position (i.e. where branch_mask & mod_type) 
   for both overwriting and removal we want to make sure we are overwriting
   or removing parts within the branch mask
*/
// assumes map_len is len, not len + 1. be careful. 
static u32 get_random_modifiable_posn(
    afl_state_t *afl,
    u32 num_to_modify,
    u8 mod_type,
    u32 map_len,
    u8* branch_mask,
    u32 * position_map
    ){
  u32 ret = 0xffffffff;
  u32 position_map_len = 0;
  int prev_start_of_1_block = -1;
  int in_0_block = 1;
  for (u32 i = 0; i < map_len; i ++){
    if (branch_mask[i] & mod_type){
      // if the last thing we saw was a zero, set
      // to start of 1 block
      if (in_0_block) {
        prev_start_of_1_block = i;
        in_0_block = 0;
      }
    } else {
      // for the first 0 we see (unless the eff_map starts with zeroes)
      // we know the last index was the last 1 in the line
      if ((!in_0_block) &&(prev_start_of_1_block != -1)){
        int num_bytes = MAX(num_to_modify/8, (u32) 1);
        for (u32 j = prev_start_of_1_block; j < i-num_bytes + 1; j++){
            // I hate this ++ within operator stuff
            position_map[position_map_len++] = j;
        }

      }
      in_0_block = 1;
    }
  }

  // if we ended not in a 0 block, add it in too 
  if (!in_0_block) {
    u32 num_bytes = MAX(num_to_modify/8, (u32) 1);
    for (u32 j = prev_start_of_1_block; j < map_len-num_bytes + 1; j++){
        // I hate this ++ within operator stuff
        position_map[position_map_len++] = j;
    }
  }

  if (position_map_len){
    u32 random_pos = rand_below(afl, position_map_len);
    if (num_to_modify >= 8)
      ret =  position_map[random_pos];
    else // I think num_to_modify can only ever be 1 if it's less than 8. otherwise need trickier stuff. 
      ret = position_map[random_pos] + rand_below(afl, 8);
  } 

  return ret;
  
}

// just need a random element of branch_mask which & with 4
// assumes map_len is len, not len + 1. be careful. 
static u32 get_random_insert_posn(afl_state_t * afl, u32 map_len, u8* branch_mask, u32 * position_map){

  u32 position_map_len = 0;
  u32 ret = map_len;

  for (u32 i = 0; i <= map_len; i++){
    if (branch_mask[i] & 4)
      position_map[position_map_len++] = i;
  }

  if (position_map_len){
    ret = position_map[rand_below(afl, position_map_len)];
  }

  return ret;
}

/* Take the current entry from the queue, fuzz it for a while. This
   function is a tad too long... returns 0 if fuzzed successfully, 1 if
   skipped or bailed out. */

u8 fuzz_one_original(afl_state_t *afl) {

  u32 len, temp_len;
  u32 j;
  u32 i;
  u8 *in_buf, *out_buf, *orig_in, *ex_tmp, *eff_map = 0;
  u64 havoc_queued = 0, orig_hit_cnt, new_hit_cnt = 0, prev_cksum;
  u32 splice_cycle = 0, perf_score = 100, orig_perf, eff_cnt = 1;

  u8 ret_val = 1, doing_det = 0;

  u8  a_collect[MAX_AUTO_EXTRA];
  u32 a_len = 0;

  /* RB Vars*/
  u8 * branch_mask = 0;
  u8 * orig_branch_mask = 0;
  u8 rb_skip_deterministic = 0;
  u8 skip_simple_bitflip = 0;
  u8 * virgin_virgin_bits = 0;
  char * shadow_prefix = "";
  u32 * position_map = NULL;
  u32 orig_queued_with_cov = afl->queued_with_cov;
  u32 orig_queued_discovered = afl->queued_discovered;
  u32 orig_total_execs = afl->fsrv.total_execs;
  

  if (!afl->vanilla_afl){
    if (afl->prev_cycle_wo_new && afl->bootstrap){
      afl->vanilla_afl = 1;
      afl->rb_fuzzing = 0;
      if (afl->bootstrap == 2){
        afl->skip_deterministic_bootstrap = 1;

      }
    }

  }

 if (afl->skip_deterministic){
  rb_skip_deterministic = 1;
  skip_simple_bitflip = 1;
 }


#ifdef IGNORE_FINDS

  /* In IGNORE_FINDS mode, skip any entries that weren't in the
     initial data set. */

  if (afl->queue_cur->depth > 1) return 1;

#else

  // Bandit FIXME: does this need to be altered for fairfuzz?

  if (unlikely(afl->custom_mutators_count)) {

    /* The custom mutator will decide to skip this test case or not. */

    LIST_FOREACH(&afl->custom_mutator_list, struct custom_mutator, {

      if (el->afl_custom_queue_get &&
          !el->afl_custom_queue_get(el->data, afl->queue_cur->fname)) {

        return 1;

      }

    });

  }

  if (afl->vanilla_afl) {

    if (likely(afl->pending_favored)) {

      /* If we have any favored, non-fuzzed new arrivals in the queue,
         possibly skip to them at the expense of already-fuzzed or non-favored
         cases. */

      if (((afl->queue_cur->was_fuzzed > 0 || afl->queue_cur->fuzz_level > 0) ||
           !afl->queue_cur->favored) &&
          likely(rand_below(afl, 100) < SKIP_TO_NEW_PROB)) {

        return 1;

      }

    } else if (!afl->non_instrumented_mode && !afl->queue_cur->favored &&

               afl->queued_paths > 10) {

      /* Otherwise, still possibly skip non-favored cases, albeit less often.
         The odds of skipping stuff are higher for already-fuzzed inputs and
         lower for never-fuzzed entries. */

      if (afl->queue_cycle > 1 &&
          (afl->queue_cur->fuzz_level == 0 || afl->queue_cur->was_fuzzed)) {

        if (likely(rand_below(afl, 100) < SKIP_NFAV_NEW_PROB)) { return 1; }

      } else {

        if (likely(rand_below(afl, 100) < SKIP_NFAV_OLD_PROB)) { return 1; }

      }

    }

  }

#endif                                                     /* ^IGNORE_FINDS */

  /* select inputs which hit rare branches */
  if (!afl->vanilla_afl) {
    afl->skip_deterministic_bootstrap = 0;
    u32 * min_branch_hits = is_rb_hit_mini(afl, afl->queue_cur->trace_mini);

    if (min_branch_hits == NULL){
      // not a rare hit. don't fuzz.
      return 1;
    } else { 
      int ii;
      for (ii = 0; min_branch_hits[ii] != 0; ii++){
        afl->rb_fuzzing = min_branch_hits[ii];
        if (afl->rb_fuzzing){
          int byte_offset = (afl->rb_fuzzing - 1) >> 3;
          int bit_offset = (afl->rb_fuzzing - 1) & 7;

          // skip deterministic if we have fuzzed this min branch
          if (afl->queue_cur->fuzzed_branches[byte_offset] & (1 << (bit_offset))){
            // let's try the next one
            continue;
          } else {
            for (u32 k = 0; k < afl->fsrv.map_size >> 3; k ++){
              if (afl->queue_cur->fuzzed_branches[k] != 0){
                DEBUG1(afl, "We fuzzed this guy already\n");
                skip_simple_bitflip = 1;
                break;
              }
            }
            // indicate we have fuzzed this branch id
            afl->queue_cur->fuzzed_branches[byte_offset] |= (1 << (bit_offset)); 
            // chose minimum
            break;
          }
        } else break; 
      }
      // if we got to the end of min_branch_hits...
      // it's either because we fuzzed all the things in min_branch_hits
      // or because there was nothing. If there was nothing, 
      // min_branch_hits[0] should be 0 
      if (!afl->rb_fuzzing || (min_branch_hits[ii] == 0)){
        afl->rb_fuzzing = min_branch_hits[0];
        if (!afl->rb_fuzzing) {
          return 1;
        }
        DEBUG1(afl, "We fuzzed this guy already for real\n");
        skip_simple_bitflip = 1;
        rb_skip_deterministic = 1;
      }
      ck_free(min_branch_hits);

    if (!skip_simple_bitflip){
      afl->cycle_wo_new = 0; 
    }
    //rarest_branches = get_lowest_hit_branch_ids();
    //DEBUG1("---\ncurrent rarest branches: ");
    //for (int k = 0; rarest_branches[k] != -1 ; k++){
    //  DEBUG1("%i (%u) ", rarest_branches[k], hit_bits[rarest_branches[k]]);
    //}
    //DEBUG1("\n");

    DEBUG1(afl, "Trying to fuzz input %s: \n", afl->queue_cur->fname);
    //for (int k = 0; k < len; k++) DEBUG1("%c", out_buf[k]);
    //DEBUG1("\n");


    DEBUG1(afl, "which hit branch %i (hit by %u inputs) \n", afl->rb_fuzzing -1, afl->hit_bits[afl->rb_fuzzing -1]);
    //ck_free(rarest_branches);
   
    }
  }

  if (unlikely(afl->not_on_tty)) {

    ACTF(
        "Fuzzing test case #%u (%u total, %llu uniq crashes found, "
        "perf_score=%0.0f, exec_us=%llu, hits=%u, map=%u)...",
        afl->current_entry, afl->queued_paths, afl->unique_crashes,
        afl->queue_cur->perf_score, afl->queue_cur->exec_us,
        likely(afl->n_fuzz) ? afl->n_fuzz[afl->queue_cur->n_fuzz_entry] : 0,
        afl->queue_cur->bitmap_size);
    fflush(stdout);

  }

  orig_in = in_buf = queue_testcase_get(afl, afl->queue_cur);
  len = afl->queue_cur->len;

  out_buf = afl_realloc(AFL_BUF_PARAM(out), len);
  if (unlikely(!out_buf)) { PFATAL("alloc"); }

  afl->subseq_tmouts = 0;

  afl->cur_depth = afl->queue_cur->depth;

  /*******************************************
   * CALIBRATION (only if failed earlier on) *
   *******************************************/

  if (unlikely(afl->queue_cur->cal_failed)) {

    u8 res = FSRV_RUN_TMOUT;

    if (afl->queue_cur->cal_failed < CAL_CHANCES) {

      afl->queue_cur->exec_cksum = 0;

      res =
          calibrate_case(afl, afl->queue_cur, in_buf, afl->queue_cycle - 1, 0);

      if (unlikely(res == FSRV_RUN_ERROR)) {

        FATAL("Unable to execute target application");

      }

    }

    if (unlikely(afl->stop_soon) || res != afl->crash_mode) {

      ++afl->cur_skipped_paths;
      goto abandon_entry;

    }

  }

  /************
   * TRIMMING *
   ************/

  if (unlikely(!afl->non_instrumented_mode && !afl->queue_cur->trim_done &&
               !afl->disable_trim)) {

    u32 old_len = afl->queue_cur->len;

    u8 res = trim_case(afl, afl->queue_cur, in_buf);
    orig_in = in_buf = queue_testcase_get(afl, afl->queue_cur);

    if (unlikely(res == FSRV_RUN_ERROR)) {

      FATAL("Unable to execute target application");

    }

    if (unlikely(afl->stop_soon)) {

      ++afl->cur_skipped_paths;
      goto abandon_entry;

    }

    /* Don't retry trimming, even if it failed. */

    afl->queue_cur->trim_done = 1;

    len = afl->queue_cur->len;

    /* maybe current entry is not ready for splicing anymore */
    if (unlikely(len <= 4 && old_len > 4)) --afl->ready_for_splicing_count;

  }

  /***************
  *  @RB@ TRIM  *
  ***************/

  u32 orig_bitmap_size = afl->queue_cur->bitmap_size;
  u64 orig_exec_us = afl->queue_cur->exec_us;

  if (afl->rb_fuzzing && afl->trim_for_branch) {

    u32 trim_len = trim_case_rb(afl, in_buf, len, out_buf);
    if (trim_len > 0){
      len = trim_len;
      /* this is kind of an unfair time measurement because the
         one in calibrate includes a lot of other loop stuff*/
      u64 start_time = get_cur_time_us();
      write_to_testcase(afl, in_buf, len);
      afl_fsrv_run_target(&afl->fsrv, afl->fsrv.exec_tmout, &afl->stop_soon);
      /* we are setting these to get a more accurate performance score */
      afl->queue_cur->exec_us = get_cur_time_us() - start_time;
      afl->queue_cur->bitmap_size = count_bytes(afl, afl->fsrv.trace_bits);

    }

  }

  memcpy(out_buf, in_buf, len);

  /*********************
   * PERFORMANCE SCORE *
   *********************/

#if 0
  if (likely(!afl->old_seed_selection))
    orig_perf = perf_score = afl->queue_cur->perf_score;
  else
#endif
  afl->queue_cur->perf_score = orig_perf = perf_score =
      calculate_score(afl, afl->queue_cur);
  /* @RB@ */
  orig_total_execs = afl->fsrv.total_execs;

  if (afl->rb_fuzzing && afl->trim_for_branch){
    /* restoring these because the changes to the test case 
     were not permanent */
    afl->queue_cur->bitmap_size = orig_bitmap_size;
    afl->queue_cur->exec_us =  orig_exec_us;
  }


  /* @RB@ */
re_run: // re-run when running in shadow mode
  if (afl->rb_fuzzing){
    if (afl->run_with_shadow && !afl->shadow_mode){
      afl->shadow_mode = 1;
      virgin_virgin_bits = ck_alloc(afl->fsrv.map_size);
      memcpy(virgin_virgin_bits, afl->virgin_bits, afl->fsrv.map_size);
      shadow_prefix = "PLAIN AFL: ";
    } else if (afl->run_with_shadow && afl->shadow_mode) {
      // reset all stats. nothing is added to queue.  
      afl->shadow_mode = 0;
      afl->queued_discovered = orig_queued_discovered;
      afl->queued_with_cov = orig_queued_with_cov;
      perf_score = orig_perf; //NOTE: this line is not stricly necessary. 
      afl->fsrv.total_execs = orig_total_execs;
      memcpy(afl->virgin_bits, virgin_virgin_bits, afl->fsrv.map_size);
      ck_free(virgin_virgin_bits);
      shadow_prefix = "RB: ";
    }

  }

  // @RB@: allocate the branch mask

  if (afl->vanilla_afl || afl->shadow_mode || (afl->use_branch_mask == 0)){
      branch_mask = alloc_branch_mask(len + 1);
      orig_branch_mask = alloc_branch_mask(len + 1);
  } else {
      branch_mask = ck_alloc(len + 1);
      orig_branch_mask = ck_alloc(len + 1);
  }
  // this will be used to store the valid modifiable positions
  // in the havoc stage. malloc'ing once to reduce overhead. 
  position_map = ck_alloc(sizeof(u32) * (len+1));

  ///////////////////// TODO: should we get rid of any of this???

  if (unlikely(perf_score <= 0)) { goto abandon_entry; }

  if (unlikely(afl->shm.cmplog_mode &&
               afl->queue_cur->colorized < afl->cmplog_lvl &&
               (u32)len <= afl->cmplog_max_filesize)) {

    if (unlikely(len < 4)) {

      afl->queue_cur->colorized = CMPLOG_LVL_MAX;

    } else {

      if (afl->cmplog_lvl == 3 ||
          (afl->cmplog_lvl == 2 && afl->queue_cur->tc_ref) ||
          afl->queue_cur->favored ||
          !(afl->fsrv.total_execs % afl->queued_paths) ||
          get_cur_time() - afl->last_path_time > 300000) {  // 300 seconds

        if (input_to_state_stage(afl, in_buf, out_buf, len)) {

          goto abandon_entry;

        }

      }

    }

  }
  ////////////////////////////////////////

  /* Skip right away if -d is given, if it has not been chosen sufficiently
     often to warrant the expensive deterministic stage (fuzz_level), or
     if it has gone through deterministic testing in earlier, resumed runs
     (passed_det). */

  if ((!afl->rb_fuzzing && afl->skip_deterministic)
      || afl->skip_deterministic_bootstrap
      || (afl->vanilla_afl && afl->queue_cur->was_fuzzed)
      || (afl->vanilla_afl && afl->queue_cur->passed_det)
      // TODO: need this????????????????????????????????????????????????????
      // TODO set rb_skip_deterministic = 1?
      || (afl->vanilla_afl && perf_score <
             (afl->queue_cur->depth * 30 <= afl->havoc_max_mult * 100
                  ? afl->queue_cur->depth * 30
                  : afl->havoc_max_mult * 100))) {

    goto custom_mutator_stage;

  }

  /* Skip deterministic fuzzing if exec path checksum puts this out of scope
     for this main instance. */

  if (unlikely(afl->main_node_max &&
               (afl->queue_cur->exec_cksum % afl->main_node_max) !=
                   afl->main_node_id - 1)) {

    if (!afl->rb_fuzzing || afl->shadow_mode)
      goto custom_mutator_stage;
    // skip all but branch mask creation if we're RB fuzzing
    else {
      rb_skip_deterministic = 1;
      skip_simple_bitflip = 1;
    }

  }


  /* Skip simple bitflip if we've done it already */
  if (skip_simple_bitflip) {
    new_hit_cnt = afl->queued_paths + afl->unique_crashes;
    goto skip_simple_bitflip;
  }


  doing_det = 1;

  /*********************************************
   * SIMPLE BITFLIP (+dictionary construction) *
   *********************************************/

#define FLIP_BIT(_ar, _b)                   \
  do {                                      \
                                            \
    u8 *_arf = (u8 *)(_ar);                 \
    u32 _bf = (_b);                         \
    _arf[(_bf) >> 3] ^= (128 >> ((_bf)&7)); \
                                            \
  } while (0)

  /* Single walking bit. */

  afl->stage_short = "flip1";
  afl->stage_max = len << 3;
  afl->stage_name = "bitflip 1/1";

  afl->stage_val_type = STAGE_VAL_NONE;

  orig_hit_cnt = afl->queued_paths + afl->unique_crashes;

  prev_cksum = afl->queue_cur->exec_cksum;

  for (afl->stage_cur = 0; afl->stage_cur < afl->stage_max; ++afl->stage_cur) {

    afl->stage_cur_byte = afl->stage_cur >> 3;

    FLIP_BIT(out_buf, afl->stage_cur);

#ifdef INTROSPECTION
    snprintf(afl->mutation, sizeof(afl->mutation), "%s FLIP_BIT1-%u",
             afl->queue_cur->fname, afl->stage_cur);
#endif

    if (common_fuzz_stuff(afl, out_buf, len)) { goto abandon_entry; }

    FLIP_BIT(out_buf, afl->stage_cur);

    /* While flipping the least significant bit in every byte, pull of an extra
       trick to detect possible syntax tokens. In essence, the idea is that if
       you have a binary blob like this:

       xxxxxxxxIHDRxxxxxxxx

       ...and changing the leading and trailing bytes causes variable or no
       changes in program flow, but touching any character in the "IHDR" string
       always produces the same, distinctive path, it's highly likely that
       "IHDR" is an atomically-checked magic value of special significance to
       the fuzzed format.

       We do this here, rather than as a separate stage, because it's a nice
       way to keep the operation approximately "free" (i.e., no extra execs).

       Empirically, performing the check when flipping the least significant bit
       is advantageous, compared to doing it at the time of more disruptive
       changes, where the program flow may be affected in more violent ways.

       The caveat is that we won't generate dictionaries in the -d mode or -S
       mode - but that's probably a fair trade-off.

       This won't work particularly well with paths that exhibit variable
       behavior, but fails gracefully, so we'll carry out the checks anyway.

      */

    if (!afl->non_instrumented_mode && (afl->stage_cur & 7) == 7) {

      u64 cksum = hash64(afl->fsrv.trace_bits, afl->fsrv.map_size, HASH_CONST);

      if (afl->stage_cur == afl->stage_max - 1 && cksum == prev_cksum) {

        /* If at end of file and we are still collecting a string, grab the
           final character and force output. */

        if (a_len < MAX_AUTO_EXTRA) {

          a_collect[a_len] = out_buf[afl->stage_cur >> 3];

        }

        ++a_len;

        if (a_len >= MIN_AUTO_EXTRA && a_len <= MAX_AUTO_EXTRA) {

          maybe_add_auto(afl, a_collect, a_len);

        }

      } else if (cksum != prev_cksum) {

        /* Otherwise, if the checksum has changed, see if we have something
           worthwhile queued up, and collect that if the answer is yes. */

        if (a_len >= MIN_AUTO_EXTRA && a_len <= MAX_AUTO_EXTRA) {

          maybe_add_auto(afl, a_collect, a_len);

        }

        a_len = 0;
        prev_cksum = cksum;

      }

      /* Continue collecting string, but only if the bit flip actually made
         any difference - we don't want no-op tokens. */

      if (cksum != afl->queue_cur->exec_cksum) {

        if (a_len < MAX_AUTO_EXTRA) {

          a_collect[a_len] = out_buf[afl->stage_cur >> 3];

        }

        ++a_len;

      }

    }

  }

  new_hit_cnt = afl->queued_paths + afl->unique_crashes;

  afl->stage_finds[STAGE_FLIP1] += new_hit_cnt - orig_hit_cnt;
  afl->stage_cycles[STAGE_FLIP1] += afl->stage_max;

  /* @RB@ */
  DEBUG1(afl, "%swhile bitflipping, %i of %i tries hit branch %i\n",
      shadow_prefix, afl->successful_branch_tries, afl->total_branch_tries, afl->rb_fuzzing - 1);

skip_simple_bitflip:

  afl->successful_branch_tries = 0;
  afl->total_branch_tries = 0;

  /* Effector map setup. These macros calculate:

     EFF_APOS      - position of a particular file offset in the map.
     EFF_ALEN      - length of a map with a particular number of bytes.
     EFF_SPAN_ALEN - map span for a sequence of bytes.

   */

#define EFF_APOS(_p) ((_p) >> EFF_MAP_SCALE2)
#define EFF_REM(_x) ((_x) & ((1 << EFF_MAP_SCALE2) - 1))
#define EFF_ALEN(_l) (EFF_APOS(_l) + !!EFF_REM(_l))
#define EFF_SPAN_ALEN(_p, _l) (EFF_APOS((_p) + (_l)-1) - EFF_APOS(_p) + 1)

  /* Initialize effector map for the next step (see comments below). Always
     flag first and last byte as doing something. */

  eff_map = afl_realloc(AFL_BUF_PARAM(eff), EFF_ALEN(len));
  memset(eff_map, 0, EFF_ALEN(len));
  if (unlikely(!eff_map)) { PFATAL("alloc"); }
  eff_map[0] = 1;

  if (EFF_APOS(len - 1) != 0) {

    eff_map[EFF_APOS(len - 1)] = 1;
    ++eff_cnt;

  }

  /* Walking byte. */

  afl->stage_name = "bitflip 8/8";
  afl->stage_short = "flip8";
  afl->stage_max = len;

  orig_hit_cnt = new_hit_cnt;

  for (afl->stage_cur = 0; afl->stage_cur < afl->stage_max; ++afl->stage_cur) {

    afl->stage_cur_byte = afl->stage_cur;

    out_buf[afl->stage_cur] ^= 0xFF;

#ifdef INTROSPECTION
    snprintf(afl->mutation, sizeof(afl->mutation), "%s FLIP_BIT8-%u",
             afl->queue_cur->fname, afl->stage_cur);
#endif

    if (common_fuzz_stuff(afl, out_buf, len)) { goto abandon_entry; }

    if (afl->rb_fuzzing && !afl->shadow_mode && afl->use_branch_mask > 0)
      if (hits_branch(afl, afl->rb_fuzzing - 1))
        branch_mask[afl->stage_cur] = 1;

    /* We also use this stage to pull off a simple trick: we identify
       bytes that seem to have no effect on the current execution path
       even when fully flipped - and we skip them during more expensive
       deterministic stages, such as arithmetics or known ints. */

    if (!eff_map[EFF_APOS(afl->stage_cur)]) {

      u64 cksum;

      /* If in non-instrumented mode or if the file is very short, just flag
         everything without wasting time on checksums. */

      if (!afl->non_instrumented_mode && len >= EFF_MIN_LEN) {

        cksum = hash64(afl->fsrv.trace_bits, afl->fsrv.map_size, HASH_CONST);

      } else {

        cksum = ~afl->queue_cur->exec_cksum;

      }

      if (cksum != afl->queue_cur->exec_cksum) {

        eff_map[EFF_APOS(afl->stage_cur)] = 1;
        ++eff_cnt;

      }

    }

    out_buf[afl->stage_cur] ^= 0xFF;

  }

  /* If the effector map is more than EFF_MAX_PERC dense, just flag the
     whole thing as worth fuzzing, since we wouldn't be saving much time
     anyway. */

  if (eff_cnt != (u32)EFF_ALEN(len) &&
      eff_cnt * 100 / EFF_ALEN(len) > EFF_MAX_PERC) {

    memset(eff_map, 1, EFF_ALEN(len));

    afl->blocks_eff_select += EFF_ALEN(len);

  } else {

    afl->blocks_eff_select += eff_cnt;

  }

  afl->blocks_eff_total += EFF_ALEN(len);

  new_hit_cnt = afl->queued_paths + afl->unique_crashes;

  afl->stage_finds[STAGE_FLIP8] += new_hit_cnt - orig_hit_cnt;
  afl->stage_cycles[STAGE_FLIP8] += afl->stage_max;
  /////////////////////////////////

  /* @RB@ also figure out add/delete map in this stage */
  if (afl->rb_fuzzing && !afl->shadow_mode && afl->use_branch_mask > 0){
    
    // buffer to clobber with new things
    u8* tmp_buf = ck_alloc(len+1);

    // check if we can delete this byte
    afl->stage_short = "rbrem8";
    for (afl->stage_cur = 0; afl->stage_cur < len; afl->stage_cur++) {
      /* delete current byte */
      afl->stage_cur_byte = afl->stage_cur;
    
      /* head */
      memcpy(tmp_buf, afl->out_buf, afl->stage_cur);
      /* tail */
      memcpy(tmp_buf + afl->stage_cur, afl->out_buf + 1 + afl->stage_cur, len - afl->stage_cur - 1 );

      if (common_fuzz_stuff(afl, tmp_buf, len - 1)) goto abandon_entry;

      /* if even with this byte deleted we hit the branch, can delete here */
      if (hits_branch(afl, afl->rb_fuzzing - 1)){
        branch_mask[afl->stage_cur] += 2;
      }
    }

    // check if we can add at this byte
    afl->stage_short = "rbadd8";
    for (afl->stage_cur = 0; afl->stage_cur <= len; afl->stage_cur++) {
      /* add random byte */
      afl->stage_cur_byte = afl->stage_cur;
      /* head */
      memcpy(tmp_buf, afl->out_buf, afl->stage_cur);
      tmp_buf[afl->stage_cur] = rand_below(afl, 256);
      /* tail */
      memcpy(tmp_buf + afl->stage_cur + 1, afl->out_buf + afl->stage_cur, len - afl->stage_cur);

      if (common_fuzz_stuff(afl, tmp_buf, len + 1)) goto abandon_entry;

      /* if adding before still hit branch, can add */
      if (hits_branch(afl, afl->rb_fuzzing - 1)){
        branch_mask[afl->stage_cur] += 4;
      }

    }

    ck_free(tmp_buf);
    // save the original branch mask for after the havoc stage 
    memcpy (orig_branch_mask, branch_mask, len + 1);
  }

  if (afl->rb_fuzzing && (afl->successful_branch_tries == 0)){
    if (afl->blacklist_pos >= afl->blacklist_size -1){
      DEBUG1(afl, "Increasing size of blacklist from %d to %d\n", afl->blacklist_size, afl->blacklist_size*2);
      afl->blacklist_size = 2 * afl->blacklist_size; 
      afl->blacklist = ck_realloc(afl->blacklist, sizeof(int) * afl->blacklist_size);
      if (!afl->blacklist){
        PFATAL("Failed to realloc blacklist");
      }
    }
    afl->blacklist[afl->blacklist_pos++] = afl->rb_fuzzing -1;
    afl->blacklist[afl->blacklist_pos] = -1;
    DEBUG1(afl, "adding branch %i to blacklist\n", afl->rb_fuzzing-1);
  }
  /* @RB@ reset stats for debugging*/
  DEBUG1(afl, "%swhile calibrating, %i of %i tries hit branch %i\n",
      shadow_prefix, afl->successful_branch_tries, afl->total_branch_tries, afl->rb_fuzzing - 1);
  DEBUG1(afl, "%scalib stage: %i new coverage in %i total execs\n",
      shadow_prefix, afl->queued_discovered - orig_queued_discovered, afl->fsrv.total_execs - orig_total_execs);
  DEBUG1(afl, "%scalib stage: %i new branches in %i total execs\n",
      shadow_prefix, afl->queued_with_cov - orig_queued_with_cov, afl->fsrv.total_execs - orig_total_execs);
  afl->successful_branch_tries = 0;
  afl->total_branch_tries = 0;

  // @RB@ TODO: skip to havoc (or dictionary add?) if can't modify any bytes 

  if (rb_skip_deterministic) goto custom_mutator_stage;

  ///////////////////////////////////
 
  /* Two walking bits. */

  afl->stage_name = "bitflip 2/1";
  afl->stage_short = "flip2";
  afl->stage_max = (len << 3) - 1;

  orig_hit_cnt = new_hit_cnt;

  for (afl->stage_cur = 0; afl->stage_cur < afl->stage_max; ++afl->stage_cur) {

    afl->stage_cur_byte = afl->stage_cur >> 3;

    FLIP_BIT(out_buf, afl->stage_cur);
    FLIP_BIT(out_buf, afl->stage_cur + 1);

#ifdef INTROSPECTION
    snprintf(afl->mutation, sizeof(afl->mutation), "%s FLIP_BIT2-%u",
             afl->queue_cur->fname, afl->stage_cur);
#endif

    if (common_fuzz_stuff(afl, out_buf, len)) { goto abandon_entry; }

    FLIP_BIT(out_buf, afl->stage_cur);
    FLIP_BIT(out_buf, afl->stage_cur + 1);

  }

  new_hit_cnt = afl->queued_paths + afl->unique_crashes;

  afl->stage_finds[STAGE_FLIP2] += new_hit_cnt - orig_hit_cnt;
  afl->stage_cycles[STAGE_FLIP2] += afl->stage_max;

  /* Four walking bits. */

  afl->stage_name = "bitflip 4/1";
  afl->stage_short = "flip4";
  afl->stage_max = (len << 3) - 3;

  orig_hit_cnt = new_hit_cnt;

  for (afl->stage_cur = 0; afl->stage_cur < afl->stage_max; ++afl->stage_cur) {

    afl->stage_cur_byte = afl->stage_cur >> 3;

    FLIP_BIT(out_buf, afl->stage_cur);
    FLIP_BIT(out_buf, afl->stage_cur + 1);
    FLIP_BIT(out_buf, afl->stage_cur + 2);
    FLIP_BIT(out_buf, afl->stage_cur + 3);

#ifdef INTROSPECTION
    snprintf(afl->mutation, sizeof(afl->mutation), "%s FLIP_BIT4-%u",
             afl->queue_cur->fname, afl->stage_cur);
#endif

    if (common_fuzz_stuff(afl, out_buf, len)) { goto abandon_entry; }

    FLIP_BIT(out_buf, afl->stage_cur);
    FLIP_BIT(out_buf, afl->stage_cur + 1);
    FLIP_BIT(out_buf, afl->stage_cur + 2);
    FLIP_BIT(out_buf, afl->stage_cur + 3);

  }

  new_hit_cnt = afl->queued_paths + afl->unique_crashes;

  afl->stage_finds[STAGE_FLIP4] += new_hit_cnt - orig_hit_cnt;
  afl->stage_cycles[STAGE_FLIP4] += afl->stage_max;


  /* Two walking bytes. */

  if (len < 2) { goto skip_bitflip; }

  afl->stage_name = "bitflip 16/8";
  afl->stage_short = "flip16";
  afl->stage_cur = 0;
  afl->stage_max = len - 1;

  orig_hit_cnt = new_hit_cnt;

  for (i = 0; i < len - 1; ++i) {

    /* Let's consult the effector map... */

    if (!eff_map[EFF_APOS(i)] && !eff_map[EFF_APOS(i + 1)]) {

      --afl->stage_max;
      continue;

    }

    if (!afl->rb_fuzzing){
      // skip if either byte will modify the branch
      if (!(branch_mask[i] & 1) || !(branch_mask[i+1] & 1) ){
        afl->stage_max--;
        continue;
      }
    }

    afl->stage_cur_byte = i;

    *(u16 *)(out_buf + i) ^= 0xFFFF;

#ifdef INTROSPECTION
    snprintf(afl->mutation, sizeof(afl->mutation), "%s FLIP_BIT16-%u",
             afl->queue_cur->fname, afl->stage_cur);
#endif

    if (common_fuzz_stuff(afl, out_buf, len)) { goto abandon_entry; }
    ++afl->stage_cur;

    *(u16 *)(out_buf + i) ^= 0xFFFF;

  }

  new_hit_cnt = afl->queued_paths + afl->unique_crashes;

  afl->stage_finds[STAGE_FLIP16] += new_hit_cnt - orig_hit_cnt;
  afl->stage_cycles[STAGE_FLIP16] += afl->stage_max;

  if (len < 4) { goto skip_bitflip; }

  /* Four walking bytes. */

  afl->stage_name = "bitflip 32/8";
  afl->stage_short = "flip32";
  afl->stage_cur = 0;
  afl->stage_max = len - 3;

  orig_hit_cnt = new_hit_cnt;

  for (i = 0; i < len - 3; ++i) {

    /* Let's consult the effector map... */
    if (!eff_map[EFF_APOS(i)] && !eff_map[EFF_APOS(i + 1)] &&
        !eff_map[EFF_APOS(i + 2)] && !eff_map[EFF_APOS(i + 3)]) {

      --afl->stage_max;
      continue;

    }

    if (afl->rb_fuzzing){
      // skip if either byte will modify the branch
      if (!(branch_mask[i] & 1) || !(branch_mask[i+1]& 1) ||
            !(branch_mask[i+2]& 1) || !(branch_mask[i+3]& 1) ){
        afl->stage_max--;
        continue;
      }
    }

    afl->stage_cur_byte = i;

    *(u32 *)(out_buf + i) ^= 0xFFFFFFFF;

#ifdef INTROSPECTION
    snprintf(afl->mutation, sizeof(afl->mutation), "%s FLIP_BIT32-%u",
             afl->queue_cur->fname, afl->stage_cur);
#endif

    if (common_fuzz_stuff(afl, out_buf, len)) { goto abandon_entry; }
    ++afl->stage_cur;

    *(u32 *)(out_buf + i) ^= 0xFFFFFFFF;

  }

  new_hit_cnt = afl->queued_paths + afl->unique_crashes;

  afl->stage_finds[STAGE_FLIP32] += new_hit_cnt - orig_hit_cnt;
  afl->stage_cycles[STAGE_FLIP32] += afl->stage_max;

skip_bitflip:

  if (afl->no_arith) { goto skip_arith; }

  /**********************
   * ARITHMETIC INC/DEC *
   **********************/

  /* 8-bit arithmetics. */

  afl->stage_name = "arith 8/8";
  afl->stage_short = "arith8";
  afl->stage_cur = 0;
  afl->stage_max = 2 * len * ARITH_MAX;

  afl->stage_val_type = STAGE_VAL_LE;

  orig_hit_cnt = new_hit_cnt;

  for (i = 0; i < (u32)len; ++i) {

    u8 orig = out_buf[i];

    /* Let's consult the effector map... */

    if (!eff_map[EFF_APOS(i)]) {

      afl->stage_max -= 2 * ARITH_MAX;
      continue;

    }

    if (afl->rb_fuzzing){
      if (!(branch_mask[i]& 1) ){
        afl->stage_max -= 2 * ARITH_MAX;
        continue;
      }
    }

    afl->stage_cur_byte = i;

    for (j = 1; j <= ARITH_MAX; ++j) {

      u8 r = orig ^ (orig + j);

      /* Do arithmetic operations only if the result couldn't be a product
         of a bitflip. */

      if (!could_be_bitflip(r)) {

        afl->stage_cur_val = j;
        out_buf[i] = orig + j;

#ifdef INTROSPECTION
        snprintf(afl->mutation, sizeof(afl->mutation), "%s ARITH8+-%u-%u",
                 afl->queue_cur->fname, i, j);
#endif

        if (common_fuzz_stuff(afl, out_buf, len)) { goto abandon_entry; }
        ++afl->stage_cur;

      } else {

        --afl->stage_max;

      }

      r = orig ^ (orig - j);

      if (!could_be_bitflip(r)) {

        afl->stage_cur_val = -j;
        out_buf[i] = orig - j;

#ifdef INTROSPECTION
        snprintf(afl->mutation, sizeof(afl->mutation), "%s ARITH8--%u-%u",
                 afl->queue_cur->fname, i, j);
#endif

        if (common_fuzz_stuff(afl, out_buf, len)) { goto abandon_entry; }
        ++afl->stage_cur;

      } else {

        --afl->stage_max;

      }

      out_buf[i] = orig;

    }

  }

  new_hit_cnt = afl->queued_paths + afl->unique_crashes;

  afl->stage_finds[STAGE_ARITH8] += new_hit_cnt - orig_hit_cnt;
  afl->stage_cycles[STAGE_ARITH8] += afl->stage_max;

  /* 16-bit arithmetics, both endians. */

  if (len < 2) { goto skip_arith; }

  afl->stage_name = "arith 16/8";
  afl->stage_short = "arith16";
  afl->stage_cur = 0;
  afl->stage_max = 4 * (len - 1) * ARITH_MAX;

  orig_hit_cnt = new_hit_cnt;

  for (i = 0; i < (u32)len - 1; ++i) {

    u16 orig = *(u16 *)(out_buf + i);

    /* Let's consult the effector map... */

    if (!eff_map[EFF_APOS(i)] && !eff_map[EFF_APOS(i + 1)]) {

      afl->stage_max -= 4 * ARITH_MAX;
      continue;

    }

    if (afl->rb_fuzzing){
      if (!(branch_mask[i] & 1) || !(branch_mask[i+1] & 1)){
        afl->stage_max -= 4 * ARITH_MAX;
        continue;
      }
    }

    afl->stage_cur_byte = i;

    for (j = 1; j <= ARITH_MAX; ++j) {

      u16 r1 = orig ^ (orig + j), r2 = orig ^ (orig - j),
          r3 = orig ^ SWAP16(SWAP16(orig) + j),
          r4 = orig ^ SWAP16(SWAP16(orig) - j);

      /* Try little endian addition and subtraction first. Do it only
         if the operation would affect more than one byte (hence the
         & 0xff overflow checks) and if it couldn't be a product of
         a bitflip. */

      afl->stage_val_type = STAGE_VAL_LE;

      if ((orig & 0xff) + j > 0xff && !could_be_bitflip(r1)) {

        afl->stage_cur_val = j;
        *(u16 *)(out_buf + i) = orig + j;

#ifdef INTROSPECTION
        snprintf(afl->mutation, sizeof(afl->mutation), "%s ARITH16+-%u-%u",
                 afl->queue_cur->fname, i, j);
#endif

        if (common_fuzz_stuff(afl, out_buf, len)) { goto abandon_entry; }
        ++afl->stage_cur;

      } else {

        --afl->stage_max;

      }

      if ((orig & 0xff) < j && !could_be_bitflip(r2)) {

        afl->stage_cur_val = -j;
        *(u16 *)(out_buf + i) = orig - j;

#ifdef INTROSPECTION
        snprintf(afl->mutation, sizeof(afl->mutation), "%s ARITH16--%u-%u",
                 afl->queue_cur->fname, i, j);
#endif

        if (common_fuzz_stuff(afl, out_buf, len)) { goto abandon_entry; }
        ++afl->stage_cur;

      } else {

        --afl->stage_max;

      }

      /* Big endian comes next. Same deal. */

      afl->stage_val_type = STAGE_VAL_BE;

      if ((orig >> 8) + j > 0xff && !could_be_bitflip(r3)) {

        afl->stage_cur_val = j;
        *(u16 *)(out_buf + i) = SWAP16(SWAP16(orig) + j);

#ifdef INTROSPECTION
        snprintf(afl->mutation, sizeof(afl->mutation), "%s ARITH16+BE-%u-%u",
                 afl->queue_cur->fname, i, j);
#endif

        if (common_fuzz_stuff(afl, out_buf, len)) { goto abandon_entry; }
        ++afl->stage_cur;

      } else {

        --afl->stage_max;

      }

      if ((orig >> 8) < j && !could_be_bitflip(r4)) {

        afl->stage_cur_val = -j;
        *(u16 *)(out_buf + i) = SWAP16(SWAP16(orig) - j);

#ifdef INTROSPECTION
        snprintf(afl->mutation, sizeof(afl->mutation), "%s ARITH16_BE-%u-%u",
                 afl->queue_cur->fname, i, j);
#endif

        if (common_fuzz_stuff(afl, out_buf, len)) { goto abandon_entry; }
        ++afl->stage_cur;

      } else {

        --afl->stage_max;

      }

      *(u16 *)(out_buf + i) = orig;

    }

  }

  new_hit_cnt = afl->queued_paths + afl->unique_crashes;

  afl->stage_finds[STAGE_ARITH16] += new_hit_cnt - orig_hit_cnt;
  afl->stage_cycles[STAGE_ARITH16] += afl->stage_max;

  /* 32-bit arithmetics, both endians. */

  if (len < 4) { goto skip_arith; }

  afl->stage_name = "arith 32/8";
  afl->stage_short = "arith32";
  afl->stage_cur = 0;
  afl->stage_max = 4 * (len - 3) * ARITH_MAX;

  orig_hit_cnt = new_hit_cnt;

  for (i = 0; i < (u32)len - 3; ++i) {

    u32 orig = *(u32 *)(out_buf + i);

    /* Let's consult the effector map... */

    if (!eff_map[EFF_APOS(i)] && !eff_map[EFF_APOS(i + 1)] &&
        !eff_map[EFF_APOS(i + 2)] && !eff_map[EFF_APOS(i + 3)]) {

      afl->stage_max -= 4 * ARITH_MAX;
      continue;

    }

    if (afl->rb_fuzzing ){
      // skip if either byte will modify the branch
      if (!(branch_mask[i] & 1) || !(branch_mask[i+1]& 1) ||
            !(branch_mask[i+2]& 1) || !(branch_mask[i+3]& 1)){
        afl->stage_max -= 4 * ARITH_MAX;
        continue;
      }
    }

    afl->stage_cur_byte = i;

    for (j = 1; j <= ARITH_MAX; ++j) {

      u32 r1 = orig ^ (orig + j), r2 = orig ^ (orig - j),
          r3 = orig ^ SWAP32(SWAP32(orig) + j),
          r4 = orig ^ SWAP32(SWAP32(orig) - j);

      /* Little endian first. Same deal as with 16-bit: we only want to
         try if the operation would have effect on more than two bytes. */

      afl->stage_val_type = STAGE_VAL_LE;

      if ((orig & 0xffff) + j > 0xffff && !could_be_bitflip(r1)) {

        afl->stage_cur_val = j;
        *(u32 *)(out_buf + i) = orig + j;

#ifdef INTROSPECTION
        snprintf(afl->mutation, sizeof(afl->mutation), "%s ARITH32+-%u-%u",
                 afl->queue_cur->fname, i, j);
#endif

        if (common_fuzz_stuff(afl, out_buf, len)) { goto abandon_entry; }
        ++afl->stage_cur;

      } else {

        --afl->stage_max;

      }

      if ((orig & 0xffff) < (u32)j && !could_be_bitflip(r2)) {

        afl->stage_cur_val = -j;
        *(u32 *)(out_buf + i) = orig - j;

#ifdef INTROSPECTION
        snprintf(afl->mutation, sizeof(afl->mutation), "%s ARITH32_-%u-%u",
                 afl->queue_cur->fname, i, j);
#endif

        if (common_fuzz_stuff(afl, out_buf, len)) { goto abandon_entry; }
        ++afl->stage_cur;

      } else {

        --afl->stage_max;

      }

      /* Big endian next. */

      afl->stage_val_type = STAGE_VAL_BE;

      if ((SWAP32(orig) & 0xffff) + j > 0xffff && !could_be_bitflip(r3)) {

        afl->stage_cur_val = j;
        *(u32 *)(out_buf + i) = SWAP32(SWAP32(orig) + j);

#ifdef INTROSPECTION
        snprintf(afl->mutation, sizeof(afl->mutation), "%s ARITH32+BE-%u-%u",
                 afl->queue_cur->fname, i, j);
#endif

        if (common_fuzz_stuff(afl, out_buf, len)) { goto abandon_entry; }
        ++afl->stage_cur;

      } else {

        --afl->stage_max;

      }

      if ((SWAP32(orig) & 0xffff) < (u32)j && !could_be_bitflip(r4)) {

        afl->stage_cur_val = -j;
        *(u32 *)(out_buf + i) = SWAP32(SWAP32(orig) - j);

#ifdef INTROSPECTION
        snprintf(afl->mutation, sizeof(afl->mutation), "%s ARITH32_BE-%u-%u",
                 afl->queue_cur->fname, i, j);
#endif

        if (common_fuzz_stuff(afl, out_buf, len)) { goto abandon_entry; }
        ++afl->stage_cur;

      } else {

        --afl->stage_max;

      }

      *(u32 *)(out_buf + i) = orig;

    }

  }

  new_hit_cnt = afl->queued_paths + afl->unique_crashes;

  afl->stage_finds[STAGE_ARITH32] += new_hit_cnt - orig_hit_cnt;
  afl->stage_cycles[STAGE_ARITH32] += afl->stage_max;

skip_arith:

  /**********************
   * INTERESTING VALUES *
   **********************/

  afl->stage_name = "interest 8/8";
  afl->stage_short = "int8";
  afl->stage_cur = 0;
  afl->stage_max = len * sizeof(interesting_8);

  afl->stage_val_type = STAGE_VAL_LE;

  orig_hit_cnt = new_hit_cnt;

  /* Setting 8-bit integers. */

  for (i = 0; i < (u32)len; ++i) {

    u8 orig = out_buf[i];

    /* Let's consult the effector map... */

    if (!eff_map[EFF_APOS(i)]) {

      afl->stage_max -= sizeof(interesting_8);
      continue;

    }

    if (afl->rb_fuzzing ){
      if (!(branch_mask[i]& 1)){
        afl->stage_max -= sizeof(interesting_8);
        continue;
      }
    }

    afl->stage_cur_byte = i;

    for (j = 0; j < (u32)sizeof(interesting_8); ++j) {

      /* Skip if the value could be a product of bitflips or arithmetics. */

      if (could_be_bitflip(orig ^ (u8)interesting_8[j]) ||
          could_be_arith(orig, (u8)interesting_8[j], 1)) {

        --afl->stage_max;
        continue;

      }

      afl->stage_cur_val = interesting_8[j];
      out_buf[i] = interesting_8[j];

#ifdef INTROSPECTION
      snprintf(afl->mutation, sizeof(afl->mutation), "%s INTERESTING8_%u_%u",
               afl->queue_cur->fname, i, j);
#endif

      if (common_fuzz_stuff(afl, out_buf, len)) { goto abandon_entry; }

      out_buf[i] = orig;
      ++afl->stage_cur;

    }

  }

  new_hit_cnt = afl->queued_paths + afl->unique_crashes;

  afl->stage_finds[STAGE_INTEREST8] += new_hit_cnt - orig_hit_cnt;
  afl->stage_cycles[STAGE_INTEREST8] += afl->stage_max;

  /* Setting 16-bit integers, both endians. */

  if (afl->no_arith || len < 2) { goto skip_interest; }

  afl->stage_name = "interest 16/8";
  afl->stage_short = "int16";
  afl->stage_cur = 0;
  afl->stage_max = 2 * (len - 1) * (sizeof(interesting_16) >> 1);

  orig_hit_cnt = new_hit_cnt;

  for (i = 0; i < len - 1; ++i) {

    u16 orig = *(u16 *)(out_buf + i);

    /* Let's consult the effector map... */

    if (!eff_map[EFF_APOS(i)] && !eff_map[EFF_APOS(i + 1)]) {

      afl->stage_max -= sizeof(interesting_16);
      continue;

    }


    if (afl->rb_fuzzing ){
      // skip if either byte will modify the branch
      if (!(branch_mask[i] & 1) || !(branch_mask[i+1] & 1)){
        afl->stage_max -= sizeof(interesting_16);
        continue;
      }
    }


    afl->stage_cur_byte = i;

    for (j = 0; j < sizeof(interesting_16) / 2; ++j) {

      afl->stage_cur_val = interesting_16[j];

      /* Skip if this could be a product of a bitflip, arithmetics,
         or single-byte interesting value insertion. */

      if (!could_be_bitflip(orig ^ (u16)interesting_16[j]) &&
          !could_be_arith(orig, (u16)interesting_16[j], 2) &&
          !could_be_interest(orig, (u16)interesting_16[j], 2, 0)) {

        afl->stage_val_type = STAGE_VAL_LE;

        *(u16 *)(out_buf + i) = interesting_16[j];

#ifdef INTROSPECTION
        snprintf(afl->mutation, sizeof(afl->mutation), "%s INTERESTING16_%u_%u",
                 afl->queue_cur->fname, i, j);
#endif

        if (common_fuzz_stuff(afl, out_buf, len)) { goto abandon_entry; }
        ++afl->stage_cur;

      } else {

        --afl->stage_max;

      }

      if ((u16)interesting_16[j] != SWAP16(interesting_16[j]) &&
          !could_be_bitflip(orig ^ SWAP16(interesting_16[j])) &&
          !could_be_arith(orig, SWAP16(interesting_16[j]), 2) &&
          !could_be_interest(orig, SWAP16(interesting_16[j]), 2, 1)) {

        afl->stage_val_type = STAGE_VAL_BE;

#ifdef INTROSPECTION
        snprintf(afl->mutation, sizeof(afl->mutation),
                 "%s INTERESTING16BE_%u_%u", afl->queue_cur->fname, i, j);
#endif

        *(u16 *)(out_buf + i) = SWAP16(interesting_16[j]);
        if (common_fuzz_stuff(afl, out_buf, len)) { goto abandon_entry; }
        ++afl->stage_cur;

      } else {

        --afl->stage_max;

      }

    }

    *(u16 *)(out_buf + i) = orig;

  }

  new_hit_cnt = afl->queued_paths + afl->unique_crashes;

  afl->stage_finds[STAGE_INTEREST16] += new_hit_cnt - orig_hit_cnt;
  afl->stage_cycles[STAGE_INTEREST16] += afl->stage_max;

  if (len < 4) { goto skip_interest; }

  /* Setting 32-bit integers, both endians. */

  afl->stage_name = "interest 32/8";
  afl->stage_short = "int32";
  afl->stage_cur = 0;
  afl->stage_max = 2 * (len - 3) * (sizeof(interesting_32) >> 2);

  orig_hit_cnt = new_hit_cnt;

  for (i = 0; i < len - 3; i++) {

    u32 orig = *(u32 *)(out_buf + i);

    /* Let's consult the effector map... */

    if (!eff_map[EFF_APOS(i)] && !eff_map[EFF_APOS(i + 1)] &&
        !eff_map[EFF_APOS(i + 2)] && !eff_map[EFF_APOS(i + 3)]) {

      afl->stage_max -= sizeof(interesting_32) >> 1;
      continue;

    }

    if (afl->rb_fuzzing ){
      // skip if any byte will modify the branch
      if (!(branch_mask[i] & 1) || !(branch_mask[i+1]& 1) ||
            !(branch_mask[i+2]& 1) || !(branch_mask[i+3]& 1)){
        afl->stage_max -= sizeof(interesting_32) >> 1;
        continue;
      }
    }

    afl->stage_cur_byte = i;

    for (j = 0; j < sizeof(interesting_32) / 4; ++j) {

      afl->stage_cur_val = interesting_32[j];

      /* Skip if this could be a product of a bitflip, arithmetics,
         or word interesting value insertion. */

      if (!could_be_bitflip(orig ^ (u32)interesting_32[j]) &&
          !could_be_arith(orig, interesting_32[j], 4) &&
          !could_be_interest(orig, interesting_32[j], 4, 0)) {

        afl->stage_val_type = STAGE_VAL_LE;

        *(u32 *)(out_buf + i) = interesting_32[j];

#ifdef INTROSPECTION
        snprintf(afl->mutation, sizeof(afl->mutation), "%s INTERESTING32_%u_%u",
                 afl->queue_cur->fname, i, j);
#endif

        if (common_fuzz_stuff(afl, out_buf, len)) { goto abandon_entry; }
        ++afl->stage_cur;

      } else {

        --afl->stage_max;

      }

      if ((u32)interesting_32[j] != SWAP32(interesting_32[j]) &&
          !could_be_bitflip(orig ^ SWAP32(interesting_32[j])) &&
          !could_be_arith(orig, SWAP32(interesting_32[j]), 4) &&
          !could_be_interest(orig, SWAP32(interesting_32[j]), 4, 1)) {

        afl->stage_val_type = STAGE_VAL_BE;

#ifdef INTROSPECTION
        snprintf(afl->mutation, sizeof(afl->mutation),
                 "%s INTERESTING32BE_%u_%u", afl->queue_cur->fname, i, j);
#endif

        *(u32 *)(out_buf + i) = SWAP32(interesting_32[j]);
        if (common_fuzz_stuff(afl, out_buf, len)) { goto abandon_entry; }
        ++afl->stage_cur;

      } else {

        --afl->stage_max;

      }

    }

    *(u32 *)(out_buf + i) = orig;

  }

  new_hit_cnt = afl->queued_paths + afl->unique_crashes;

  afl->stage_finds[STAGE_INTEREST32] += new_hit_cnt - orig_hit_cnt;
  afl->stage_cycles[STAGE_INTEREST32] += afl->stage_max;

skip_interest:

  /********************
   * DICTIONARY STUFF *
   ********************/

  if (!afl->extras_cnt) { goto skip_user_extras; }

  /* Overwrite with user-supplied extras. */

  afl->stage_name = "user extras (over)";
  afl->stage_short = "ext_UO";
  afl->stage_cur = 0;
  afl->stage_max = afl->extras_cnt * len;

  afl->stage_val_type = STAGE_VAL_NONE;

  orig_hit_cnt = new_hit_cnt;

  for (i = 0; i < (u32)len; ++i) {

    u32 last_len = 0;

    afl->stage_cur_byte = i;

    /* Extras are sorted by size, from smallest to largest. This means
       that we don't have to worry about restoring the buffer in
       between writes at a particular offset determined by the outer
       loop. */

    for (j = 0; j < afl->extras_cnt; ++j) {

      /* Skip extras probabilistically if afl->extras_cnt > AFL_MAX_DET_EXTRAS.
         Also skip them if there's no room to insert the payload, if the token
         is redundant, or if its entire span has no bytes set in the effector
         map. */

      if ((afl->extras_cnt > afl->max_det_extras &&
           rand_below(afl, afl->extras_cnt) >= afl->max_det_extras) ||
          afl->extras[j].len > len - i ||
          !memcmp(afl->extras[j].data, out_buf + i, afl->extras[j].len) ||
          !memchr(eff_map + EFF_APOS(i), 1,
                  EFF_SPAN_ALEN(i, afl->extras[j].len))) {

        --afl->stage_max;
        continue;

      }
 
      if (afl->rb_fuzzing ){//&& use_mask()){
      // if any fall outside the mask, skip
        int bailing = 0;
        for (u32 ii = 0; ii < afl->extras[j].len; ii ++){
          if (!(branch_mask[i + ii] & 1)){
            bailing = 1;
            break;
          }

        }
        if (bailing){
          afl->stage_max--;
          continue;
        }        
      }

      last_len = afl->extras[j].len;
      memcpy(out_buf + i, afl->extras[j].data, last_len);

#ifdef INTROSPECTION
      snprintf(afl->mutation, sizeof(afl->mutation),
               "%s EXTRAS_overwrite-%u-%u", afl->queue_cur->fname, i, j);
#endif

      if (common_fuzz_stuff(afl, out_buf, len)) { goto abandon_entry; }

      ++afl->stage_cur;

    }

    /* Restore all the clobbered memory. */
    memcpy(out_buf + i, in_buf + i, last_len);

  }

  new_hit_cnt = afl->queued_paths + afl->unique_crashes;

  afl->stage_finds[STAGE_EXTRAS_UO] += new_hit_cnt - orig_hit_cnt;
  afl->stage_cycles[STAGE_EXTRAS_UO] += afl->stage_max;

  /* Insertion of user-supplied extras. */

  afl->stage_name = "user extras (insert)";
  afl->stage_short = "ext_UI";
  afl->stage_cur = 0;
  afl->stage_max = afl->extras_cnt * (len + 1);

  orig_hit_cnt = new_hit_cnt;

  ex_tmp = afl_realloc(AFL_BUF_PARAM(ex), len + MAX_DICT_FILE);
  if (unlikely(!ex_tmp)) { PFATAL("alloc"); }

  for (i = 0; i <= (u32)len; ++i) {

    afl->stage_cur_byte = i;

    for (j = 0; j < afl->extras_cnt; ++j) {

      if (len + afl->extras[j].len > MAX_FILE) {

        --afl->stage_max;
        continue;

      }

      // consult insert map....
      if (!(branch_mask[i] & 4) ){
        afl->stage_max--;
        continue;
      }

      /* Insert token */
      memcpy(ex_tmp + i, afl->extras[j].data, afl->extras[j].len);

      /* Copy tail */
      memcpy(ex_tmp + i + afl->extras[j].len, out_buf + i, len - i);

#ifdef INTROSPECTION
      snprintf(afl->mutation, sizeof(afl->mutation), "%s EXTRAS_insert-%u-%u",
               afl->queue_cur->fname, i, j);
#endif

      if (common_fuzz_stuff(afl, ex_tmp, len + afl->extras[j].len)) {

        goto abandon_entry;

      }

      ++afl->stage_cur;

    }

    /* Copy head */
    ex_tmp[i] = out_buf[i];

  }

  new_hit_cnt = afl->queued_paths + afl->unique_crashes;

  afl->stage_finds[STAGE_EXTRAS_UI] += new_hit_cnt - orig_hit_cnt;
  afl->stage_cycles[STAGE_EXTRAS_UI] += afl->stage_max;

skip_user_extras:

  if (!afl->a_extras_cnt) { goto skip_extras; }

  afl->stage_name = "auto extras (over)";
  afl->stage_short = "ext_AO";
  afl->stage_cur = 0;
  afl->stage_max = MIN(afl->a_extras_cnt, (u32)USE_AUTO_EXTRAS) * len;

  afl->stage_val_type = STAGE_VAL_NONE;

  orig_hit_cnt = new_hit_cnt;

  for (i = 0; i < (u32)len; ++i) {

    u32 last_len = 0;

    afl->stage_cur_byte = i;

    u32 min_extra_len = MIN(afl->a_extras_cnt, (u32)USE_AUTO_EXTRAS);
    for (j = 0; j < min_extra_len; ++j) {

      /* See the comment in the earlier code; extras are sorted by size. */

      if (afl->a_extras[j].len > len - i ||
          !memcmp(afl->a_extras[j].data, out_buf + i, afl->a_extras[j].len) ||
          !memchr(eff_map + EFF_APOS(i), 1,
                  EFF_SPAN_ALEN(i, afl->a_extras[j].len))) {

        --afl->stage_max;
        continue;

      }


      // if any fall outside the mask, skip
      if (afl->rb_fuzzing){ 
      // if any fall outside the mask, skip
        int bailing = 0;
        for (u32 ii = 0; ii < afl->a_extras[j].len; ii ++){
          if (!(branch_mask[i + ii] & 1)){
            bailing = 1;
            break;
          }

        }
        if (bailing){
          afl->stage_max--;
          continue;
        }        
      }


      last_len = afl->a_extras[j].len;
      memcpy(out_buf + i, afl->a_extras[j].data, last_len);

#ifdef INTROSPECTION
      snprintf(afl->mutation, sizeof(afl->mutation),
               "%s AUTO_EXTRAS_overwrite-%u-%u", afl->queue_cur->fname, i, j);
#endif

      if (common_fuzz_stuff(afl, out_buf, len)) { goto abandon_entry; }

      ++afl->stage_cur;

    }

    /* Restore all the clobbered memory. */
    memcpy(out_buf + i, in_buf + i, last_len);

  }

  new_hit_cnt = afl->queued_paths + afl->unique_crashes;

  afl->stage_finds[STAGE_EXTRAS_AO] += new_hit_cnt - orig_hit_cnt;
  afl->stage_cycles[STAGE_EXTRAS_AO] += afl->stage_max;

skip_extras:

  /* If we made this to here without jumping to havoc_stage or abandon_entry,
     we're properly done with deterministic steps and can mark it as such
     in the .state/ directory. */

  if (!afl->queue_cur->passed_det) { mark_as_det_done(afl, afl->queue_cur); }

  /* @RB@ reset stats for debugging*/
  DEBUG1(afl, "%sIn deterministic stage, %i of %i tries hit branch %i\n",
      shadow_prefix, afl->successful_branch_tries, afl->total_branch_tries, afl->rb_fuzzing - 1);
  DEBUG1(afl, "%sdet stage: %i new coverage in %i total execs\n",
      shadow_prefix, afl->queued_discovered - orig_queued_discovered, afl->fsrv.total_execs - orig_total_execs);
  DEBUG1(afl, "%sdet stage: %i new branches in %i total execs\n",
      shadow_prefix, afl->queued_with_cov - orig_queued_with_cov, afl->fsrv.total_execs - orig_total_execs);

  afl->successful_branch_tries = 0;
  afl->total_branch_tries = 0;

custom_mutator_stage:
  /*******************
   * CUSTOM MUTATORS *
   *******************/

  if (likely(!afl->custom_mutators_count)) { goto havoc_stage; }

  afl->stage_name = "custom mutator";
  afl->stage_short = "custom";
  afl->stage_max = HAVOC_CYCLES * perf_score / afl->havoc_div / 100;
  afl->stage_val_type = STAGE_VAL_NONE;
  bool has_custom_fuzz = false;

  if (afl->stage_max < HAVOC_MIN) { afl->stage_max = HAVOC_MIN; }

  const u32 max_seed_size = MAX_FILE, saved_max = afl->stage_max;

  orig_hit_cnt = afl->queued_paths + afl->unique_crashes;

#ifdef INTROSPECTION
  afl->mutation[0] = 0;
#endif

  LIST_FOREACH(&afl->custom_mutator_list, struct custom_mutator, {

    if (el->afl_custom_fuzz) {

      afl->current_custom_fuzz = el;

      if (el->afl_custom_fuzz_count) {

        afl->stage_max = el->afl_custom_fuzz_count(el->data, out_buf, len);

      } else {

        afl->stage_max = saved_max;

      }

      has_custom_fuzz = true;

      afl->stage_short = el->name_short;

      if (afl->stage_max) {

        for (afl->stage_cur = 0; afl->stage_cur < afl->stage_max;
             ++afl->stage_cur) {

          struct queue_entry *target = NULL;
          u32                 tid;
          u8 *                new_buf = NULL;
          u32                 target_len = 0;

          /* check if splicing makes sense yet (enough entries) */
          if (likely(afl->ready_for_splicing_count > 1)) {

            /* Pick a random other queue entry for passing to external API
               that has the necessary length */

            do {

              tid = rand_below(afl, afl->queued_paths);

            } while (unlikely(tid == afl->current_entry ||

                              afl->queue_buf[tid]->len < 4));

            target = afl->queue_buf[tid];
            afl->splicing_with = tid;

            /* Read the additional testcase into a new buffer. */
            new_buf = queue_testcase_get(afl, target);
            target_len = target->len;

          }

          u8 *mutated_buf = NULL;

          size_t mutated_size =
              el->afl_custom_fuzz(el->data, out_buf, len, &mutated_buf, new_buf,
                                  target_len, max_seed_size);

          if (unlikely(!mutated_buf)) {

            FATAL("Error in custom_fuzz. Size returned: %zu", mutated_size);

          }

          if (mutated_size > 0) {

            if (common_fuzz_stuff(afl, mutated_buf, (u32)mutated_size)) {

              goto abandon_entry;

            }

            if (!el->afl_custom_fuzz_count) {

              /* If we're finding new stuff, let's run for a bit longer, limits
                permitting. */

              if (afl->queued_paths != havoc_queued) {

                if (perf_score <= afl->havoc_max_mult * 100) {

                  afl->stage_max *= 2;
                  perf_score *= 2;

                }

                havoc_queued = afl->queued_paths;

              }

            }

          }

          /* `(afl->)out_buf` may have been changed by the call to custom_fuzz
           */
          /* TODO: Only do this when `mutated_buf` == `out_buf`? Branch vs
           * Memcpy.
           */
          memcpy(out_buf, in_buf, len);

        }

      }

    }

  });

  afl->current_custom_fuzz = NULL;

  if (!has_custom_fuzz) goto havoc_stage;

  new_hit_cnt = afl->queued_paths + afl->unique_crashes;

  afl->stage_finds[STAGE_CUSTOM_MUTATOR] += new_hit_cnt - orig_hit_cnt;
  afl->stage_cycles[STAGE_CUSTOM_MUTATOR] += afl->stage_max;

  if (likely(afl->custom_only)) {

    /* Skip other stages */
    ret_val = 0;
    goto abandon_entry;

  }

  /****************
   * RANDOM HAVOC *
   ****************/

havoc_stage:
   
  // @RB@ TODO: don't havoc if there's nothing to modify :()

  afl->stage_cur_byte = -1;

  /* The havoc stage mutation code is also invoked when splicing files; if the
     splice_cycle variable is set, generate different descriptions and such. */

  if (!splice_cycle) {

    afl->stage_name = "havoc";
    afl->stage_short = "havoc";
    afl->stage_max = (doing_det ? HAVOC_CYCLES_INIT : HAVOC_CYCLES) *
                     perf_score / afl->havoc_div / 100;

  } else {

    perf_score = orig_perf;

    snprintf(afl->stage_name_buf, STAGE_BUF_SIZE, "splice %u", splice_cycle);
    afl->stage_name = afl->stage_name_buf;
    afl->stage_short = "splice";
    afl->stage_max = SPLICE_HAVOC * perf_score / afl->havoc_div / 100;

  }

  if (afl->stage_max < HAVOC_MIN) { afl->stage_max = HAVOC_MIN; }

  temp_len = len;
  position_map = ck_realloc(position_map, sizeof (u32) * (temp_len + 1));

  orig_hit_cnt = afl->queued_paths + afl->unique_crashes;

  havoc_queued = afl->queued_paths;

  if (afl->custom_mutators_count) {

    LIST_FOREACH(&afl->custom_mutator_list, struct custom_mutator, {

      if (el->stacked_custom && el->afl_custom_havoc_mutation_probability) {

        el->stacked_custom_prob =
            el->afl_custom_havoc_mutation_probability(el->data);
        if (el->stacked_custom_prob > 100) {

          FATAL(
              "The probability returned by "
              "afl_custom_havoc_mutation_propability "
              "has to be in the range 0-100.");

        }

      }

    });

  }

  /* We essentially just do several thousand runs (depending on perf_score)
     where we take the input file and make random stacked tweaks. */

#define MAX_HAVOC_ENTRY 59                                      /* 55 to 60 */

  u32 r_max, r;

  r_max = (MAX_HAVOC_ENTRY + 1) + (afl->extras_cnt ? 4 : 0) +
          (afl->a_extras_cnt ? 4 : 0);

  if (unlikely(afl->expand_havoc && afl->ready_for_splicing_count > 1)) {

    /* add expensive havoc cases here, they are activated after a full
       cycle without finds happened */

    r_max += 4;

  }

  if (unlikely(get_cur_time() - afl->last_path_time > 5000 /* 5 seconds */ &&
               afl->ready_for_splicing_count > 1)) {

    /* add expensive havoc cases here if there is no findings in the last 5s */

    r_max += 4;

  }

  int batch_bucket = NUM_BATCH_BUCKET-1;
#if NUM_BATCH_BUCKET == 5
  if (len <= 100) {
    batch_bucket = 0;
  } else if (len <= 1000) {
    batch_bucket = 1;
  } else if (len <= 10000) {
    batch_bucket = 2;
  } else if (len <= 100000) {
    batch_bucket = 3;
  }
#endif

  int mut_bucket;
#ifdef USE_LEN_BUCKET_FOR_MOPTWISE
  mut_bucket = batch_bucket;
#else
  mut_bucket = 0;
#endif

  BANDIT_T(MUT_ALG)*    mut_bandit  = &afl->mut_bandit[mut_bucket];
  BANDIT_T(BATCH_ALG)* used_bucket = afl->batch_bandit[batch_bucket];

  for (afl->stage_cur = 0; afl->stage_cur < afl->stage_max; ++afl->stage_cur) {

#ifdef MOPTWISE_BANDIT

    int selected_case;
    u8 mask[NUM_CASE_ENUM] = { 0 };

    if (!afl->extras_cnt) {
      mask[OVERWRITE_WITH_EXTRA] = 1;
      mask[INSERT_EXTRA] = 1;
    }

    if (!afl->a_extras_cnt) {
      mask[OVERWRITE_WITH_AEXTRA] = 1;
      mask[INSERT_AEXTRA] = 1;
    }

    if (afl->ready_for_splicing_count <= 1) {
      mask[SPLICE_INSERT] = 1;
      mask[SPLICE_OVERWRITE] = 1;
    }

    if (len + HAVOC_BLK_XL >= MAX_FILE) {
      mask[SPLICE_INSERT] = 1;
    }

    if (len < 2) {
      mask[SPLICE_OVERWRITE] = 1;
    }
    
    selected_case = SELECT_ARM(MUT_ALG)(afl, mut_bandit, mask);

#if MUT_ALG == exppp || MUT_ALG == expix
    u8 exp_invalid = mask[selected_case];
    if (exp_invalid) goto L_EXP_INVALID;
#endif

    static const int case2r[] = {
      0, 4, 8, 10, 12, 14, 16, 20, 24, 26, 28, 30, 32, 34, 36, 38, 40, 44, 47, 48, 51, 52, MAX_HAVOC_ENTRY+1,
      MAX_HAVOC_ENTRY+3, MAX_HAVOC_ENTRY+5, MAX_HAVOC_ENTRY+7, MAX_HAVOC_ENTRY+10, MAX_HAVOC_ENTRY+9
    };

    r = case2r[selected_case];
    if (selected_case >= OVERWRITE_WITH_AEXTRA) {
      if (!afl->extras_cnt) r -= 4;
    }

    if (selected_case >= SPLICE_OVERWRITE) {
      if (!afl->a_extras_cnt) r -= 4;
    }

#elif defined(MOPTWISE_BANDIT_FINECOARSE) /* MOPTWISE_BANDIT */
 
    int selected_case;
    selected_case = SELECT_ARM(MUT_ALG) (afl, mut_bandit, NULL);

    if (selected_case == 0) r = rand_below(afl, 44);
    else r = 44 + rand_below(afl, r_max-44);

#else /* MOPTWISE_BANDIT */ 
    /* otherwise draw r uniformly randomly */ 
    r = rand_below(afl, r_max);
#endif

    int case_idx = 0;

#ifdef ATOMIZE_CASES
    u32 r_bkup = r;
    switch (r) {
      case 0 ... 3: {
        case_idx = FLIP_BIT1;
        break;
      }

      case 4 ... 7: {
        case_idx = INTERESTING8;
        break;
      }

      case 8 ... 9: {
        case_idx = INTERESTING16;
        break;    
      }

      case 10 ... 11: {
        case_idx = INTERESTING16BE;
        break;
      }

      case 12 ... 13: {
        case_idx = INTERESTING32;
        break;
      }

      case 14 ... 15: {
        case_idx = INTERESTING32BE;
        break;
      }

      case 16 ... 19: {
        case_idx = ARITH8_MINUS;
        break;
      }

      case 20 ... 23: {
        case_idx = ARITH8_PLUS;
        break;
      }

      case 24 ... 25: {
        case_idx = ARITH16_MINUS;
        break;
      }

      case 26 ... 27: {
        case_idx = ARITH16_BE_MINUS;
        break;
      }

      case 28 ... 29: {
        case_idx = ARITH16_PLUS;
        break;
      }

      case 30 ... 31: {
        case_idx = ARITH16_BE_PLUS;
        break;
      }

      case 32 ... 33: {
        case_idx = ARITH32_MINUS;
        break;
      }

      case 34 ... 35: {
        case_idx = ARITH32_BE_MINUS;
        break;
      }

      case 36 ... 37: {
        case_idx = ARITH32_PLUS;
        break;
      }

      case 38 ... 39: {
        case_idx = ARITH32_BE_PLUS;
        break;
      }

      case 40 ... 43: {
        case_idx = RAND8;
        break;
      }
  
      case 44 ... 46:
        case_idx = CLONE_BYTES;
        break;

      case 47:
        case_idx = INSERT_SAME_BYTE;
        break;

      case 48 ... 50:
        case_idx = OVERWRITE_WITH_CHUNK;
        break;

      case 51:
        case_idx = OVERWRITE_WITH_SAME_BYTE;
        break;

      case 52 ... MAX_HAVOC_ENTRY:
        case_idx = DELETE_BYTES;
        break;

      default:
        r -= (MAX_HAVOC_ENTRY + 1);
        if (afl->extras_cnt) {
          if (r < 2) {
            case_idx = OVERWRITE_WITH_EXTRA;
            break;
          } else if (r < 4) {
            case_idx = INSERT_EXTRA;
            break;
          } else {
            r -= 4;
          }
        }

        if (afl->a_extras_cnt) {
          if (r < 2) {
            case_idx = OVERWRITE_WITH_AEXTRA;
            break;
          } else if (r < 4) {
            case_idx = INSERT_AEXTRA;
            break;
          } else {
            r -= 4;
          }
        }

        if ((temp_len >= 2 && r % 2) || temp_len + HAVOC_BLK_XL >= MAX_FILE) {
            case_idx = SPLICE_OVERWRITE;
        } else {
            case_idx = SPLICE_INSERT;
        }

        break;
    }
    r = r_bkup;
#elif defined(DIVIDE_COARSE_FINE)
    switch (r) {
      case 0 ... 43: {
        case_idx = 0; // fine-grained
        break;
      }

      default:
        case_idx = 1; // coarse-grained
        break;
    }
#else 
    case_idx = 0;
#endif

#if defined(MOPTWISE_BANDIT) && defined(ATOMIZE_CASES)
    if (case_idx != selected_case) {
      FATAL("case_idx: %d, selected_case: %d, r: %u\n", case_idx, selected_case, r);
    }
#endif

    BANDIT_T(BATCH_ALG) *batch_bandit = &used_bucket[case_idx];

    u32 mutation_pos[512];
    u32 mutation_data32[512];

    enum MutationByteSize { OTHER, BIT1, BYTE1, BYTE2, BYTE4};
    u8  mutation_size = OTHER;

    u8 *mutation_data8 = (u8*)mutation_data32;
    u16 *mutation_data16 = (u16*)mutation_data32;
    //printf("addr~~ %p %p %p\n", mutation_data32, mutation_data8, mutation_data16);

    int selected_t = 0;
#ifndef BATCHSIZE_BANDIT
    selected_t = rand_below(afl, afl->havoc_stack_pow2+1);
#else
    selected_t = SELECT_ARM(BATCH_ALG) 
                       (afl, batch_bandit, NULL);
#endif

#if BATCH_NUM_ARM == 7
    u32 use_stacking = 1 << selected_t;
#else
    u32 use_stacking = 1 + selected_t;
#endif

    afl->stage_cur_val = use_stacking;

#ifdef INTROSPECTION
    snprintf(afl->mutation, sizeof(afl->mutation), "%s HAVOC-%u",
             afl->queue_cur->fname, use_stacking);
#endif

      if (afl->custom_mutators_count) {

        LIST_FOREACH(&afl->custom_mutator_list, struct custom_mutator, {

          if (el->stacked_custom &&
              rand_below(afl, 100) < el->stacked_custom_prob) {

            u8 *   custom_havoc_buf = NULL;
            size_t new_len = el->afl_custom_havoc_mutation(
                el->data, out_buf, temp_len, &custom_havoc_buf, MAX_FILE);
            if (unlikely(!custom_havoc_buf)) {

              FATAL("Error in custom_havoc (return %zu)", new_len);

            }

            if (likely(new_len > 0 && custom_havoc_buf)) {

              temp_len = new_len;
              position_map = ck_realloc(position_map, sizeof (u32) * (temp_len + 1));
              if (out_buf != custom_havoc_buf) {

                out_buf = afl_realloc(AFL_BUF_PARAM(out), temp_len);
                if (unlikely(!afl->out_buf)) { PFATAL("alloc"); }
                memcpy(out_buf, custom_havoc_buf, temp_len);

              }

            }

          }

        });

      }

      switch (r) {

        case 0 ... 3: {

          /* Flip a single bit somewhere. Spooky! */

#ifdef INTROSPECTION
          snprintf(afl->m_tmp, sizeof(afl->m_tmp), " FLIP_BIT1");
          strcat(afl->mutation, afl->m_tmp);
#endif
    
          mutation_size = BIT1;

          for (i = 0; i < use_stacking; ++i) {

          u32 pos = get_random_modifiable_posn(afl, 1, 1, temp_len, branch_mask, position_map);
          if (pos == 0xffffffff) { break; }
          //u32 pos = rand_below(afl, temp_len << 3);
          mutation_pos[i] = pos; // FLIP_BIT only requires position to revert it
          FLIP_BIT(out_buf, pos);
          
          }

          break;

        }

        case 4 ... 7: {

          /* Set byte to interesting value. */

#ifdef INTROSPECTION
          snprintf(afl->m_tmp, sizeof(afl->m_tmp), " INTERESTING8");
          strcat(afl->mutation, afl->m_tmp);
#endif

          mutation_size = BYTE1;

          for (i = 0; i < use_stacking; ++i) {

          u32 pos = get_random_modifiable_posn(afl, 8, 1, temp_len, branch_mask, position_map);
          if (pos == 0xffffffff) { break; }
          //u32 pos = rand_below(afl, temp_len);
          mutation_pos[i] = pos; 
          mutation_data8[i] = out_buf[pos];

          out_buf[pos] =
              interesting_8[rand_below(afl, sizeof(interesting_8))];

          }

          break;

        }

        case 8 ... 9: {

          /* Set word to interesting value, little endian. */

          mutation_size = BYTE2;

          if (temp_len < 2) { break; }

#ifdef INTROSPECTION
          snprintf(afl->m_tmp, sizeof(afl->m_tmp), " INTERESTING16");
          strcat(afl->mutation, afl->m_tmp);
#endif

          for (i = 0; i < use_stacking; ++i) {

          u32 pos = get_random_modifiable_posn(afl, 16, 1, temp_len, branch_mask, position_map);
          if (pos == 0xffffffff) { break; }
          //u32 pos = rand_below(afl, temp_len - 1);
          mutation_pos[i] = pos; 
          mutation_data16[i] = *(u16 *)(out_buf + pos);

          *(u16 *)(out_buf + pos) =
              interesting_16[rand_below(afl, sizeof(interesting_16) >> 1)];

          }

          break;

        }

        case 10 ... 11: {

          /* Set word to interesting value, big endian. */

          mutation_size = BYTE2;

          if (temp_len < 2) { break; }

#ifdef INTROSPECTION
          snprintf(afl->m_tmp, sizeof(afl->m_tmp), " INTERESTING16BE");
          strcat(afl->mutation, afl->m_tmp);
#endif

          for (i = 0; i < use_stacking; ++i) {

          u32 pos = get_random_modifiable_posn(afl, 16, 1, temp_len, branch_mask, position_map);
          if (pos == 0xffffffff) { break; }
          //u32 pos = rand_below(afl, temp_len - 1);
          mutation_pos[i] = pos; 
          mutation_data16[i] = *(u16 *)(out_buf + pos);

          *(u16 *)(out_buf + pos) = SWAP16(
              interesting_16[rand_below(afl, sizeof(interesting_16) >> 1)]);

          }

          break;

        }

        case 12 ... 13: {

          /* Set dword to interesting value, little endian. */

          mutation_size = BYTE4;

          if (temp_len < 4) { break; }

#ifdef INTROSPECTION
          snprintf(afl->m_tmp, sizeof(afl->m_tmp), " INTERESTING32");
          strcat(afl->mutation, afl->m_tmp);
#endif

          for (i = 0; i < use_stacking; ++i) {

          u32 pos = get_random_modifiable_posn(afl, 32, 1, temp_len, branch_mask, position_map);
          if (pos == 0xffffffff) { break; }
          //u32 pos = rand_below(afl, temp_len - 3);
          mutation_pos[i] = pos; 
          mutation_data32[i] = *(u32 *)(out_buf + pos);

          *(u32 *)(out_buf + pos) =
              interesting_32[rand_below(afl, sizeof(interesting_32) >> 2)];
          
          }

          break;

        }

        case 14 ... 15: {

          /* Set dword to interesting value, big endian. */

          mutation_size = BYTE4;

          if (temp_len < 4) { break; }

#ifdef INTROSPECTION
          snprintf(afl->m_tmp, sizeof(afl->m_tmp), " INTERESTING32BE");
          strcat(afl->mutation, afl->m_tmp);
#endif

          for (i = 0; i < use_stacking; ++i) {

          u32 pos = get_random_modifiable_posn(afl, 32, 1, temp_len, branch_mask, position_map);
          if (pos == 0xffffffff) { break; }
          //u32 pos = rand_below(afl, temp_len - 3);
          mutation_pos[i] = pos; 
          mutation_data32[i] = *(u32 *)(out_buf + pos);

          *(u32 *)(out_buf + pos) = SWAP32(
              interesting_32[rand_below(afl, sizeof(interesting_32) >> 2)]);

          }

          break;

        }

        case 16 ... 19: {

          /* Randomly subtract from byte. */

#ifdef INTROSPECTION
          snprintf(afl->m_tmp, sizeof(afl->m_tmp), " ARITH8_");
          strcat(afl->mutation, afl->m_tmp);
#endif

          mutation_size = BYTE1;

          for (i = 0; i < use_stacking; ++i) {

          u32 pos = get_random_modifiable_posn(afl, 8, 1, temp_len, branch_mask, position_map);
          if (pos == 0xffffffff) { break; }
          //u32 pos = rand_below(afl, temp_len);
          mutation_pos[i] = pos; 
          mutation_data8[i] = out_buf[pos];

          out_buf[pos] -= 1 + rand_below(afl, ARITH_MAX);

          }

          break;

        }

        case 20 ... 23: {

          /* Randomly add to byte. */

#ifdef INTROSPECTION
          snprintf(afl->m_tmp, sizeof(afl->m_tmp), " ARITH8+");
          strcat(afl->mutation, afl->m_tmp);
#endif

          mutation_size = BYTE1;

          for (i = 0; i < use_stacking; ++i) {

          u32 pos = get_random_modifiable_posn(afl, 8, 1, temp_len, branch_mask, position_map);
          if (pos == 0xffffffff) { break; }
          //u32 pos = rand_below(afl, temp_len);
          mutation_pos[i] = pos; 
          mutation_data8[i] = out_buf[pos];

          out_buf[pos] += 1 + rand_below(afl, ARITH_MAX);

          }

          break;

        }

        case 24 ... 25: {

          /* Randomly subtract from word, little endian. */

          mutation_size = BYTE2;

          if (temp_len < 2) { break; }

          for (i = 0; i < use_stacking; ++i) {

          u32 pos = get_random_modifiable_posn(afl, 16, 1, temp_len, branch_mask, position_map);
          if (pos == 0xffffffff) { break; }
          //u32 pos = rand_below(afl, temp_len - 1);
          mutation_pos[i] = pos; 
          mutation_data16[i] = *(u16 *)(out_buf + pos);


#ifdef INTROSPECTION
          snprintf(afl->m_tmp, sizeof(afl->m_tmp), " ARITH16_-%u", pos);
          strcat(afl->mutation, afl->m_tmp);
#endif
          *(u16 *)(out_buf + pos) -= 1 + rand_below(afl, ARITH_MAX);

          }

          break;

        }

        case 26 ... 27: {

          /* Randomly subtract from word, big endian. */

          mutation_size = BYTE2;

          if (temp_len < 2) { break; }

          for (i = 0; i < use_stacking; ++i) {

          u32 pos = get_random_modifiable_posn(afl, 16, 1, temp_len, branch_mask, position_map);
          if (pos == 0xffffffff) { break; }
          //u32 pos = rand_below(afl, temp_len - 1);
          mutation_pos[i] = pos; 
          mutation_data16[i] = *(u16 *)(out_buf + pos);
          u16 num = 1 + rand_below(afl, ARITH_MAX);

#ifdef INTROSPECTION
          snprintf(afl->m_tmp, sizeof(afl->m_tmp), " ARITH16_BE-%u_%u", pos,
                   num);
          strcat(afl->mutation, afl->m_tmp);
#endif
          *(u16 *)(out_buf + pos) =
              SWAP16(SWAP16(*(u16 *)(out_buf + pos)) - num);

          }

          break;

        }

        case 28 ... 29: {

          /* Randomly add to word, little endian. */

          mutation_size = BYTE2;

          if (temp_len < 2) { break; }

          for (i = 0; i < use_stacking; ++i) {

          u32 pos = get_random_modifiable_posn(afl, 16, 1, temp_len, branch_mask, position_map);
          if (pos == 0xffffffff) { break; }
          //u32 pos = rand_below(afl, temp_len - 1);
          mutation_pos[i] = pos; 
          mutation_data16[i] = *(u16 *)(out_buf + pos);

#ifdef INTROSPECTION
          snprintf(afl->m_tmp, sizeof(afl->m_tmp), " ARITH16+-%u", pos);
          strcat(afl->mutation, afl->m_tmp);
#endif
          *(u16 *)(out_buf + pos) += 1 + rand_below(afl, ARITH_MAX);

          }

          break;

        }

        case 30 ... 31: {

          /* Randomly add to word, big endian. */

          mutation_size = BYTE2;

          if (temp_len < 2) { break; }

          for (i = 0; i < use_stacking; ++i) {

          u32 pos = get_random_modifiable_posn(afl, 16, 1, temp_len, branch_mask, position_map);
          if (pos == 0xffffffff) { break; }
          //u32 pos = rand_below(afl, temp_len - 1);
          mutation_pos[i] = pos; 
          mutation_data16[i] = *(u16 *)(out_buf + pos);
          u16 num = 1 + rand_below(afl, ARITH_MAX);

#ifdef INTROSPECTION
          snprintf(afl->m_tmp, sizeof(afl->m_tmp), " ARITH16+BE-%u_%u", pos,
                   num);
          strcat(afl->mutation, afl->m_tmp);
#endif
          *(u16 *)(out_buf + pos) =
              SWAP16(SWAP16(*(u16 *)(out_buf + pos)) + num);

          }

          break;

        }

        case 32 ... 33: {

          /* Randomly subtract from dword, little endian. */

          mutation_size = BYTE4;

          if (temp_len < 4) { break; }

          for (i = 0; i < use_stacking; ++i) {

          u32 pos = get_random_modifiable_posn(afl, 32, 1, temp_len, branch_mask, position_map);
          if (pos == 0xffffffff) { break; }
          //u32 pos = rand_below(afl, temp_len - 3);
          mutation_pos[i] = pos; 
          mutation_data32[i] = *(u32 *)(out_buf + pos);

#ifdef INTROSPECTION
          snprintf(afl->m_tmp, sizeof(afl->m_tmp), " ARITH32_-%u", pos);
          strcat(afl->mutation, afl->m_tmp);
#endif
          *(u32 *)(out_buf + pos) -= 1 + rand_below(afl, ARITH_MAX);

          }

          break;

        }

        case 34 ... 35: {

          /* Randomly subtract from dword, big endian. */

          mutation_size = BYTE4;

          if (temp_len < 4) { break; }

          for (i = 0; i < use_stacking; ++i) {


          u32 pos = get_random_modifiable_posn(afl, 32, 1, temp_len, branch_mask, position_map);
          if (pos == 0xffffffff) { break; }
          //u32 pos = rand_below(afl, temp_len - 3);
          mutation_pos[i] = pos; 
          mutation_data32[i] = *(u32 *)(out_buf + pos);
          u32 num = 1 + rand_below(afl, ARITH_MAX);

#ifdef INTROSPECTION
          snprintf(afl->m_tmp, sizeof(afl->m_tmp), " ARITH32_BE-%u-%u", pos,
                   num);
          strcat(afl->mutation, afl->m_tmp);
#endif
          *(u32 *)(out_buf + pos) =
              SWAP32(SWAP32(*(u32 *)(out_buf + pos)) - num);

          }

          break;

        }

        case 36 ... 37: {

          /* Randomly add to dword, little endian. */

          mutation_size = BYTE4;

          if (temp_len < 4) { break; }

          for (i = 0; i < use_stacking; ++i) {


          u32 pos = get_random_modifiable_posn(afl, 32, 1, temp_len, branch_mask, position_map);
          if (pos == 0xffffffff) { break; }
          //u32 pos = rand_below(afl, temp_len - 3);
          mutation_pos[i] = pos; 
          mutation_data32[i] = *(u32 *)(out_buf + pos);

#ifdef INTROSPECTION
          snprintf(afl->m_tmp, sizeof(afl->m_tmp), " ARITH32+-%u", pos);
          strcat(afl->mutation, afl->m_tmp);
#endif
          *(u32 *)(out_buf + pos) += 1 + rand_below(afl, ARITH_MAX);

          }

          break;

        }

        case 38 ... 39: {

          /* Randomly add to dword, big endian. */

          mutation_size = BYTE4;

          if (temp_len < 4) { break; }

          for (i = 0; i < use_stacking; ++i) {


          u32 pos = get_random_modifiable_posn(afl, 32, 1, temp_len, branch_mask, position_map);
          if (pos == 0xffffffff) { break; }
          //u32 pos = rand_below(afl, temp_len - 3);
          mutation_pos[i] = pos; 
          mutation_data32[i] = *(u32 *)(out_buf + pos);
          u32 num = 1 + rand_below(afl, ARITH_MAX);

#ifdef INTROSPECTION
          snprintf(afl->m_tmp, sizeof(afl->m_tmp), " ARITH32+BE-%u-%u", pos,
                   num);
          strcat(afl->mutation, afl->m_tmp);
#endif
          *(u32 *)(out_buf + pos) =
              SWAP32(SWAP32(*(u32 *)(out_buf + pos)) + num);

          }

          break;

        }

        case 40 ... 43: {

          /* Just set a random byte to a random value. Because,
             why not. We use XOR with 1-255 to eliminate the
             possibility of a no-op. */

#ifdef INTROSPECTION
          snprintf(afl->m_tmp, sizeof(afl->m_tmp), " RAND8");
          strcat(afl->mutation, afl->m_tmp);
#endif

          mutation_size = BYTE1;

          for (i = 0; i < use_stacking; ++i) {


          u32 pos = get_random_modifiable_posn(afl, 32, 1, temp_len, branch_mask, position_map);
          if (pos == 0xffffffff) { break; }
          //u32 pos = rand_below(afl, temp_len);
          mutation_pos[i] = pos; 
          mutation_data8[i] = out_buf[pos];

          out_buf[pos] ^= 1 + rand_below(afl, 255);

          }

          break;

        }

        case 44 ... 46: {

          for (i = 0; i < use_stacking; ++i) {

          if (temp_len + HAVOC_BLK_XL < MAX_FILE) {

            /* Clone bytes. */

            u32 clone_len = choose_block_len(afl, temp_len);
            u32 clone_from = rand_below(afl, temp_len - clone_len + 1);
            u32 clone_to = get_random_insert_posn(afl, temp_len, branch_mask, position_map);
            if (clone_to == 0xffffffff) { break; }
            //u32 clone_to = rand_below(afl, temp_len);

#ifdef INTROSPECTION
            snprintf(afl->m_tmp, sizeof(afl->m_tmp), " CLONE-%s-%u-%u-%u",
                     "clone", clone_from, clone_to, clone_len);
            strcat(afl->mutation, afl->m_tmp);
#endif
            u8 *new_buf =
                afl_realloc(AFL_BUF_PARAM(out_scratch), temp_len + clone_len);
            if (unlikely(!new_buf)) { PFATAL("alloc"); }
            u8 *new_branch_mask = alloc_branch_mask(temp_len + clone_len + 1);

            /* Head */

            memcpy(new_buf, out_buf, clone_to);
            memcpy(new_branch_mask, branch_mask, clone_to);

            /* Inserted part */

            memcpy(new_buf + clone_to, out_buf + clone_from, clone_len);

            /* Tail */
            memcpy(new_buf + clone_to + clone_len, out_buf + clone_to,
                   temp_len - clone_to);
            memcpy(new_branch_mask + clone_to + clone_len, branch_mask + clone_to,
                temp_len - clone_to + 1);

            ck_free(branch_mask);

            out_buf = new_buf;
            branch_mask = new_branch_mask;
            afl_swap_bufs(AFL_BUF_PARAM(out), AFL_BUF_PARAM(out_scratch));
            temp_len += clone_len;

            position_map = ck_realloc(position_map, sizeof (u32) * (temp_len + 1));
            if (!position_map)
              PFATAL("Failure resizing position_map.\n");

          } else break;

          }

          break;

        }

        case 47: {

          for (i = 0; i < use_stacking; ++i) {

          if (temp_len + HAVOC_BLK_XL < MAX_FILE) {

            /* Insert a block of constant bytes (25%). */

            u32 clone_len = choose_block_len(afl, HAVOC_BLK_XL);
            u32 clone_to = get_random_insert_posn(afl, temp_len, branch_mask, position_map);
            if (clone_to == 0xffffffff) { break; }
            //u32 clone_to = rand_below(afl, temp_len);

#ifdef INTROSPECTION
            snprintf(afl->m_tmp, sizeof(afl->m_tmp), " CLONE-%s-%u-%u",
                     "insert", clone_to, clone_len);
            strcat(afl->mutation, afl->m_tmp);
#endif
            u8 *new_buf =
                afl_realloc(AFL_BUF_PARAM(out_scratch), temp_len + clone_len);
            if (unlikely(!new_buf)) { PFATAL("alloc"); }
            u8 *new_branch_mask = alloc_branch_mask(temp_len + clone_len + 1);

            /* Head */

            memcpy(new_buf, out_buf, clone_to);
            memcpy(new_branch_mask, branch_mask, clone_to);

            /* Inserted part */

            memset(new_buf + clone_to,
                   rand_below(afl, 2) ? rand_below(afl, 256)
                                      : out_buf[rand_below(afl, temp_len)],
                   clone_len);

            /* Tail */
            memcpy(new_buf + clone_to + clone_len, out_buf + clone_to,
                   temp_len - clone_to);
            memcpy(new_branch_mask + clone_to + clone_len, branch_mask + clone_to,
                temp_len - clone_to + 1);

            ck_free(branch_mask);

            out_buf = new_buf;
            branch_mask = new_branch_mask;
            afl_swap_bufs(AFL_BUF_PARAM(out), AFL_BUF_PARAM(out_scratch));
            temp_len += clone_len;

            position_map = ck_realloc(position_map, sizeof (u32) * (temp_len + 1));
            if (!position_map)
              PFATAL("Failure resizing position_map.\n");

          } else break;

          }

          break;

        }

        case 48 ... 50: {

          /* Overwrite bytes with a randomly selected chunk bytes. */

          if (temp_len < 2) { break; }

          for (i = 0; i < use_stacking; ++i) {

          u32 copy_len = choose_block_len(afl, temp_len - 1);
          u32 copy_from = rand_below(afl, temp_len - copy_len + 1);
          u32 copy_to = get_random_modifiable_posn(afl, copy_len * 8, 1, temp_len, branch_mask, position_map);
          if (copy_to == 0xffffffff) { break; }
          //u32 copy_to = rand_below(afl, temp_len - copy_len + 1);

          if (likely(copy_from != copy_to)) {

#ifdef INTROSPECTION
            snprintf(afl->m_tmp, sizeof(afl->m_tmp), " OVERWRITE_COPY-%u-%u-%u",
                     copy_from, copy_to, copy_len);
            strcat(afl->mutation, afl->m_tmp);
#endif
            memmove(out_buf + copy_to, out_buf + copy_from, copy_len);

          }

          }

          break;

        }

        case 51: {

          /* Overwrite bytes with fixed bytes. */

          if (temp_len < 2) { break; }

          for (i = 0; i < use_stacking; ++i) {

          u32 copy_len = choose_block_len(afl, temp_len - 1);
          u32 copy_to = get_random_modifiable_posn(afl, copy_len * 8, 1, temp_len, branch_mask, position_map);
          if (copy_to == 0xffffffff) { break; }
          //u32 copy_to = rand_below(afl, temp_len - copy_len + 1);

#ifdef INTROSPECTION
          snprintf(afl->m_tmp, sizeof(afl->m_tmp), " OVERWRITE_FIXED-%u-%u",
                   copy_to, copy_len);
          strcat(afl->mutation, afl->m_tmp);
#endif
          memset(out_buf + copy_to,
                 rand_below(afl, 2) ? rand_below(afl, 256)
                                    : out_buf[rand_below(afl, temp_len)],
                 copy_len);

          }

          break;

        }

        // increase from 4 up to 8?
        case 52 ... MAX_HAVOC_ENTRY: {

          for (i = 0; i < use_stacking; ++i) {

          /* Delete bytes. We're making this a bit more likely
             than insertion (the next option) in hopes of keeping
             files reasonably small. */

          if (temp_len < 2) { break; }

          /* Don't delete too much. */

          u32 del_len = choose_block_len(afl, temp_len - 1);
          u32 del_from = get_random_modifiable_posn(afl, del_len * 8, 2, temp_len, branch_mask, position_map);
          if (del_from == 0xffffffff) { break; }
          //u32 del_from = rand_below(afl, temp_len - del_len + 1);

#ifdef INTROSPECTION
          snprintf(afl->m_tmp, sizeof(afl->m_tmp), " DEL-%u-%u", del_from,
                   del_len);
          strcat(afl->mutation, afl->m_tmp);
#endif
          memmove(out_buf + del_from, out_buf + del_from + del_len,
                  temp_len - del_from - del_len);

          memmove(branch_mask + del_from, branch_mask + del_from + del_len,
                  temp_len - del_from - del_len + 1);

          temp_len -= del_len;

          }

          break;

        }

        default:

          r -= (MAX_HAVOC_ENTRY + 1);

          if (afl->extras_cnt) {

            if (r < 2) {

              for (i = 0; i < use_stacking; ++i) {

              /* Use the dictionary. */

              u32 use_extra = rand_below(afl, afl->extras_cnt);
              u32 extra_len = afl->extras[use_extra].len;

              if (extra_len > temp_len) { break; }


              u32 insert_at = get_random_modifiable_posn(afl, extra_len * 8, 1, temp_len, branch_mask, position_map);
              if (insert_at == 0xffffffff) { break; }
              //u32 insert_at = rand_below(afl, temp_len - extra_len + 1);

#ifdef INTROSPECTION
              snprintf(afl->m_tmp, sizeof(afl->m_tmp), " EXTRA_OVERWRITE-%u-%u",
                       insert_at, extra_len);
              strcat(afl->mutation, afl->m_tmp);
#endif
              memcpy(out_buf + insert_at, afl->extras[use_extra].data,
                     extra_len);

              }

              break;

            } else if (r < 4) {

              for (i = 0; i < use_stacking; ++i) {

              u32 use_extra = rand_below(afl, afl->extras_cnt);
              u32 extra_len = afl->extras[use_extra].len;
              if (temp_len + extra_len >= MAX_FILE) { break; }

              u8 *ptr = afl->extras[use_extra].data;
              u32 insert_at = get_random_insert_posn(afl, temp_len, branch_mask, position_map);
              if (insert_at == 0xffffffff) { break; }
              //u32 insert_at = rand_below(afl, temp_len + 1);
#ifdef INTROSPECTION
              snprintf(afl->m_tmp, sizeof(afl->m_tmp), " EXTRA_INSERT-%u-%u",
                       insert_at, extra_len);
              strcat(afl->mutation, afl->m_tmp);
#endif

              out_buf = afl_realloc(AFL_BUF_PARAM(out), temp_len + extra_len);
              if (unlikely(!out_buf)) { PFATAL("alloc"); }
              u8 *new_branch_mask = alloc_branch_mask(temp_len + extra_len + 1);

              /* Head */
              memcpy(new_branch_mask, branch_mask, insert_at);

              /* Tail */
              memmove(out_buf + insert_at + extra_len, out_buf + insert_at,
                      temp_len - insert_at);
              memcpy(new_branch_mask + insert_at + extra_len, branch_mask + insert_at,
                      temp_len - insert_at + 1);

              /* Inserted part */
              memcpy(out_buf + insert_at, ptr, extra_len);

              ck_free(branch_mask);
              branch_mask = new_branch_mask;

              temp_len += extra_len;

              position_map = ck_realloc(position_map, sizeof (u32) * (temp_len + 1));
              if (!position_map)
                PFATAL("Failure resizing position_map.\n");

              }

              break;

            } else {

              r -= 4;

            }

          }

          if (afl->a_extras_cnt) {

            if (r < 2) {

              for (i = 0; i < use_stacking; ++i) {

              /* Use the dictionary. */

              u32 use_extra = rand_below(afl, afl->a_extras_cnt);
              u32 extra_len = afl->a_extras[use_extra].len;

              if (extra_len > temp_len) { break; }

              u32 insert_at = get_random_modifiable_posn(afl, extra_len * 8, 1, temp_len, branch_mask, position_map);
              if (insert_at == 0xffffffff) { break; }
              //u32 insert_at = rand_below(afl, temp_len - extra_len + 1);
#ifdef INTROSPECTION
              snprintf(afl->m_tmp, sizeof(afl->m_tmp),
                       " AUTO_EXTRA_OVERWRITE-%u-%u", insert_at, extra_len);
              strcat(afl->mutation, afl->m_tmp);
#endif
              memcpy(out_buf + insert_at, afl->a_extras[use_extra].data,
                     extra_len);

              }

              break;

            } else if (r < 4) {

              for (i = 0; i < use_stacking; ++i) {

              u32 use_extra = rand_below(afl, afl->a_extras_cnt);
              u32 extra_len = afl->a_extras[use_extra].len;
              if (temp_len + extra_len >= MAX_FILE) { break; }

              u8 *ptr = afl->a_extras[use_extra].data;
              u32 insert_at = get_random_insert_posn(afl, temp_len, branch_mask, position_map);
              if (insert_at == 0xffffffff) { break; }
              //u32 insert_at = rand_below(afl, temp_len + 1);
#ifdef INTROSPECTION
              snprintf(afl->m_tmp, sizeof(afl->m_tmp),
                       " AUTO_EXTRA_INSERT-%u-%u", insert_at, extra_len);
              strcat(afl->mutation, afl->m_tmp);
#endif

              out_buf = afl_realloc(AFL_BUF_PARAM(out), temp_len + extra_len);
              if (unlikely(!out_buf)) { PFATAL("alloc"); }
              u8 *new_branch_mask = alloc_branch_mask(temp_len + extra_len + 1);

              /* Head */
              memcpy(new_branch_mask, branch_mask, insert_at);

              /* Tail */
              memmove(out_buf + insert_at + extra_len, out_buf + insert_at,
                      temp_len - insert_at);
              memcpy(new_branch_mask + insert_at + extra_len, branch_mask + insert_at,
                      temp_len - insert_at + 1);

              /* Inserted part */
              memcpy(out_buf + insert_at, ptr, extra_len);

              ck_free(branch_mask);
              branch_mask = new_branch_mask;

              temp_len += extra_len;

              position_map = ck_realloc(position_map, sizeof (u32) * (temp_len + 1));
              if (!position_map)
                PFATAL("Failure resizing position_map.\n");

              }

              break;

            } else {

              r -= 4;

            }

          }

          for (i = 0; i < use_stacking; ++i) {

          /* Splicing otherwise if we are still here.
             Overwrite bytes with a randomly selected chunk from another
             testcase or insert that chunk. */

          /* Pick a random queue entry and seek to it. */

          u32 tid;
          do {

            tid = rand_below(afl, afl->queued_paths);

          } while (tid == afl->current_entry || afl->queue_buf[tid]->len < 4);

          /* Get the testcase for splicing. */
          struct queue_entry *target = afl->queue_buf[tid];
          u32                 new_len = target->len;
          u8 *                new_buf = queue_testcase_get(afl, target);

          if ((temp_len >= 2 && r % 2) || temp_len + HAVOC_BLK_XL >= MAX_FILE) {

            /* overwrite mode */

            u32 copy_from, copy_to, copy_len;

            copy_len = choose_block_len(afl, new_len - 1);
            if (copy_len > temp_len) copy_len = temp_len;

            copy_from = rand_below(afl, new_len - copy_len + 1);
            copy_to = get_random_modifiable_posn(afl, copy_len * 8, 1, temp_len, branch_mask, position_map);
            if (copy_to == 0xffffffff) { break; }
            //copy_to = rand_below(afl, temp_len - copy_len + 1);

#ifdef INTROSPECTION
            snprintf(afl->m_tmp, sizeof(afl->m_tmp),
                     " SPLICE_OVERWRITE-%u-%u-%u-%s", copy_from, copy_to,
                     copy_len, target->fname);
            strcat(afl->mutation, afl->m_tmp);
#endif
            memmove(out_buf + copy_to, new_buf + copy_from, copy_len);

          } else {

            /* insert mode */

            u32 clone_from, clone_to, clone_len;

            clone_len = choose_block_len(afl, new_len);
            clone_from = rand_below(afl, new_len - clone_len + 1);
            clone_to = get_random_insert_posn(afl, temp_len, branch_mask, position_map);
            if (clone_to == 0xffffffff) { break; }
            //clone_to = rand_below(afl, temp_len + 1);

            u8 *temp_buf = afl_realloc(AFL_BUF_PARAM(out_scratch),
                                       temp_len + clone_len + 1);
            if (unlikely(!temp_buf)) { PFATAL("alloc"); }
            u8 *new_branch_mask = alloc_branch_mask(temp_len + clone_len + 1);

#ifdef INTROSPECTION
            snprintf(afl->m_tmp, sizeof(afl->m_tmp),
                     " SPLICE_INSERT-%u-%u-%u-%s", clone_from, clone_to,
                     clone_len, target->fname);
            strcat(afl->mutation, afl->m_tmp);
#endif
            /* Head */

            memcpy(temp_buf, out_buf, clone_to);
            memcpy(new_branch_mask, branch_mask, clone_to);

            /* Inserted part */

            memcpy(temp_buf + clone_to, new_buf + clone_from, clone_len);

            /* Tail */
            memcpy(temp_buf + clone_to + clone_len, out_buf + clone_to,
                   temp_len - clone_to);
            memcpy(new_branch_mask + clone_to + clone_len, branch_mask + clone_to,
                    temp_len - clone_to + 1);

            ck_free(branch_mask);

            out_buf = temp_buf;
            branch_mask = new_branch_mask;

            afl_swap_bufs(AFL_BUF_PARAM(out), AFL_BUF_PARAM(out_scratch));
            temp_len += clone_len;

            position_map = ck_realloc(position_map, sizeof (u32) * (temp_len + 1));
            if (!position_map)
              PFATAL("Failure resizing position_map.\n");

          }

          }

          break;

          // end of default

      }


#if MUT_ALG == exppp || MUT_ALG == expix
L_EXP_INVALID:
#endif

    afl->fsrv.total_havocs++;

#if MUT_ALG == exppp || MUT_ALG == expix
    if (exp_invalid) goto L_EXP_INVALID_2;
#endif

    u8 should_abandon = common_fuzz_stuff(afl, out_buf, temp_len);
    if (should_abandon) {
#ifdef BATCHSIZE_BANDIT
      ADD_REWARD(BATCH_ALG)(batch_bandit, selected_t, 0);
#endif

#if defined(MOPTWISE_BANDIT) || defined(MOPTWISE_BANDIT_FINECOARSE)
      ADD_REWARD(MUT_ALG)(mut_bandit, selected_case, 0);
#endif
      goto abandon_entry; 
    }

    /* out_buf might have been mangled a bit, so let's restore it to its
       original size and shape. */

    if (len >= MIN_LEN_FOR_OPTIMIZED_RESTORE) {

      // For fine grained mutations, we recover the data from the saved one.
      switch (mutation_size) {
        // FLIP BIT
        case BIT1: {
          int j;
          u32 pos;
          for (j = use_stacking - 1; j >= 0; j--) {
            pos = mutation_pos[j];
            FLIP_BIT(out_buf, pos);
          }
          break;
        }
  
        // case mutation size = 1 byte
        case BYTE1: {
          int j;
          u32 pos;
          u8  data;
          for (j = use_stacking - 1; j >= 0; j--) {
            pos = mutation_pos[j];
            data = mutation_data8[j];
            // printf("%u %u %u %u %u\n", mutation_id, i, r_bkup, pos, data);
            out_buf[pos] = data;
          }
          break;
        }
  
        // case mutation size = 2 bytes
        case BYTE2: {
          int j;
          u32 pos;
          u16 data;
          if (temp_len < 2) { break; }
          for (j = use_stacking - 1; j >= 0; j--) {
            pos = mutation_pos[j];
            data = mutation_data16[j];
            // printf("%u %u %u %u %u\n", mutation_id, j, r_bkup, pos, data);
            *(u16 *)(out_buf + pos) = data;
          }
          break;
        }
  
        // case mutation size = 4 bytes
        case BYTE4: {
          int j;
          u32 pos;
          u32 data;
          if (temp_len < 4) { break; }
          for (j = use_stacking - 1; j >= 0; j--) {
            pos = mutation_pos[j];
            data = mutation_data32[j];
            // printf("%u %u %u %u %u\n", mutation_id, j, r_bkup, mutation_pos[j], data);
            *(u32 *)(out_buf + pos) = data;
          }
          break;
        }
  
        default: {
          out_buf = afl_realloc(AFL_BUF_PARAM(out), len);
          if (unlikely(!out_buf)) { PFATAL("alloc"); }
          temp_len = len;
          position_map = ck_realloc(position_map, sizeof (u32) * (temp_len + 1));
          memcpy(out_buf, in_buf, len);
        }
      }
    } else {
      out_buf = afl_realloc(AFL_BUF_PARAM(out), len);
      if (unlikely(!out_buf)) { PFATAL("alloc"); }
      branch_mask = ck_realloc(branch_mask, len + 1);
      position_map = ck_realloc(position_map, sizeof (u32) * (len + 1));
      if (!branch_mask || !position_map)
        PFATAL("Failure resizing position_map.\n");
      temp_len = len;
      position_map = ck_realloc(position_map, sizeof (u32) * (temp_len + 1));
      memcpy(out_buf, in_buf, len);
      memcpy(branch_mask, orig_branch_mask, len + 1);
    }

    /* If we're finding new stuff, let's run for a bit longer, limits
       permitting. */

    if (afl->queued_paths != havoc_queued) {
#ifdef BATCHSIZE_BANDIT
      ADD_REWARD(BATCH_ALG)(batch_bandit, selected_t, 1);
#endif

#if defined(MOPTWISE_BANDIT) || defined(MOPTWISE_BANDIT_FINECOARSE)
      ADD_REWARD(MUT_ALG)(mut_bandit, selected_case, 1);
#endif

      if (perf_score <= afl->havoc_max_mult * 100) {

        afl->stage_max *= 2;
        perf_score *= 2;

      }

      havoc_queued = afl->queued_paths;
    }  else {

#ifdef BATCHSIZE_BANDIT
      ADD_REWARD(BATCH_ALG)(batch_bandit, selected_t, 0);
#endif

#if MUT_ALG == exppp || MUT_ALG == expix
L_EXP_INVALID_2:
#endif

#if defined(MOPTWISE_BANDIT) || defined(MOPTWISE_BANDIT_FINECOARSE)
      ADD_REWARD(MUT_ALG)(mut_bandit, selected_case, 0);
#endif

    }
  }

  new_hit_cnt = afl->queued_paths + afl->unique_crashes;

  if (!splice_cycle) {

    afl->stage_finds[STAGE_HAVOC] += new_hit_cnt - orig_hit_cnt;
    afl->stage_cycles[STAGE_HAVOC] += afl->stage_max;

  } else {

    afl->stage_finds[STAGE_SPLICE] += new_hit_cnt - orig_hit_cnt;
    afl->stage_cycles[STAGE_SPLICE] += afl->stage_max;

  }

#ifndef IGNORE_FINDS

  /************
   * SPLICING *
   ************/

  /* This is a last-resort strategy triggered by a full round with no findings.
     It takes the current input file, randomly selects another input, and
     splices them together at some offset, then relies on the havoc
     code to mutate that blob. */

retry_splicing:

  if (afl->use_splicing && splice_cycle++ < SPLICE_CYCLES &&
      afl->ready_for_splicing_count > 1 && afl->queue_cur->len >= 4) {

    struct queue_entry *target;
    u32                 tid, split_at;
    u8 *                new_buf;
    s32                 f_diff, l_diff;

    /* First of all, if we've modified in_buf for havoc, let's clean that
       up... */

    if (in_buf != orig_in) {

      in_buf = orig_in;
      len = afl->queue_cur->len;

    }

    /* Pick a random queue entry and seek to it. Don't splice with yourself. */

    do {

      tid = rand_below(afl, afl->queued_paths);

    } while (tid == afl->current_entry || afl->queue_buf[tid]->len < 4);

    /* Get the testcase */
    afl->splicing_with = tid;
    target = afl->queue_buf[tid];
    new_buf = queue_testcase_get(afl, target);

    /* Find a suitable splicing location, somewhere between the first and
       the last differing byte. Bail out if the difference is just a single
       byte or so. */

    locate_diffs(in_buf, new_buf, MIN(len, (s64)target->len), &f_diff, &l_diff);

    if (f_diff < 0 || l_diff < 2 || f_diff == l_diff) { goto retry_splicing; }

    /* Split somewhere between the first and last differing byte. */

    split_at = f_diff + rand_below(afl, l_diff - f_diff);

    /* Do the thing. */

    len = target->len;
    afl->in_scratch_buf = afl_realloc(AFL_BUF_PARAM(in_scratch), len);
    memcpy(afl->in_scratch_buf, in_buf, split_at);
    memcpy(afl->in_scratch_buf + split_at, new_buf, len - split_at);
    in_buf = afl->in_scratch_buf;
    afl_swap_bufs(AFL_BUF_PARAM(in), AFL_BUF_PARAM(in_scratch));

    out_buf = afl_realloc(AFL_BUF_PARAM(out), len);
    if (unlikely(!out_buf)) { PFATAL("alloc"); }
    memcpy(out_buf, in_buf, len);

    // @RB@ handle the branch mask...

    u8 * new_branch_mask = alloc_branch_mask(len + 1);

    memcpy(new_branch_mask, branch_mask, MIN(split_at, temp_len + 1));
    ck_free(branch_mask);
    branch_mask = new_branch_mask;
    ck_free(orig_branch_mask);
    orig_branch_mask = ck_alloc(len +1);
    //ck_realloc(orig_branch_mask, len + 1);
    memcpy (orig_branch_mask, branch_mask, len + 1);
    position_map = ck_realloc(position_map, sizeof (u32) * (len + 1));
    if (!position_map)
      PFATAL("Failure resizing position_map.\n");

    goto custom_mutator_stage;

  }

#endif                                                     /* !IGNORE_FINDS */

  ret_val = 0;

/* we are through with this queue entry - for this iteration */
abandon_entry:

  afl->splicing_with = -1;

  /* Update afl->pending_not_fuzzed count if we made it through the calibration
     cycle and have not seen this entry before. */

  if (!afl->stop_soon && !afl->queue_cur->cal_failed &&
      (afl->queue_cur->was_fuzzed == 0 || afl->queue_cur->fuzz_level == 0) &&
      !afl->queue_cur->disabled) {

    if (!afl->queue_cur->was_fuzzed) {

      --afl->pending_not_fuzzed;
      afl->queue_cur->was_fuzzed = 1;
      afl->reinit_table = 1;
      if (afl->queue_cur->favored) { --afl->pending_favored; }

    }

  }

  /* @RB@ reset stats for debugging*/
  DEBUG1(afl, "%sIn havoc stage, %i of %i tries hit branch %i\n",
      shadow_prefix, afl->successful_branch_tries, afl->total_branch_tries, afl->rb_fuzzing - 1);
  afl->successful_branch_tries = 0;
  afl->total_branch_tries = 0;
  DEBUG1(afl, "%shavoc stage: %i new coverage in %i total execs\n",
      shadow_prefix, afl->queued_discovered - orig_queued_discovered, afl->fsrv.total_execs - orig_total_execs);
  DEBUG1(afl, "%shavoc stage: %i new branches in %i total execs\n",
      shadow_prefix, afl->queued_with_cov - orig_queued_with_cov, afl->fsrv.total_execs - orig_total_execs);
  if (afl->shadow_mode) goto re_run;

  if (afl->queued_with_cov - orig_queued_with_cov){
    afl->prev_cycle_wo_new = 0;
    afl->vanilla_afl = 0;
    afl->cycle_wo_new = 0;
  }

  ck_free(position_map);
  ck_free(branch_mask);
  ck_free(orig_branch_mask);

  ++afl->queue_cur->fuzz_level;
  orig_in = NULL;
  return ret_val;

#undef FLIP_BIT

}

/* MOpt mode */
static u8 mopt_common_fuzzing(afl_state_t *afl, MOpt_globals_t MOpt_globals) {

  if (!MOpt_globals.is_pilot_mode) {

    if (swarm_num == 1) {

      afl->key_module = 2;
      return 0;

    }

  }

  u32 len, temp_len;
  u32 i;
  u32 j;
  u8 *in_buf, *out_buf, *orig_in, *ex_tmp, *eff_map = 0;
  u64 havoc_queued = 0, orig_hit_cnt, new_hit_cnt = 0, cur_ms_lv, prev_cksum;
  u32 splice_cycle = 0, perf_score = 100, orig_perf, eff_cnt = 1;

  u8 ret_val = 1, doing_det = 0;

  u8  a_collect[MAX_AUTO_EXTRA];
  u32 a_len = 0;

#ifdef IGNORE_FINDS

  /* In IGNORE_FINDS mode, skip any entries that weren't in the
     initial data set. */

  if (afl->queue_cur->depth > 1) return 1;

#else

  if (likely(afl->pending_favored)) {

    /* If we have any favored, non-fuzzed new arrivals in the queue,
       possibly skip to them at the expense of already-fuzzed or non-favored
       cases. */

    if (((afl->queue_cur->was_fuzzed > 0 || afl->queue_cur->fuzz_level > 0) ||
         !afl->queue_cur->favored) &&
        rand_below(afl, 100) < SKIP_TO_NEW_PROB) {

      return 1;

    }

  } else if (!afl->non_instrumented_mode && !afl->queue_cur->favored &&

             afl->queued_paths > 10) {

    /* Otherwise, still possibly skip non-favored cases, albeit less often.
       The odds of skipping stuff are higher for already-fuzzed inputs and
       lower for never-fuzzed entries. */

    if (afl->queue_cycle > 1 &&
        (afl->queue_cur->fuzz_level == 0 || afl->queue_cur->was_fuzzed)) {

      if (likely(rand_below(afl, 100) < SKIP_NFAV_NEW_PROB)) { return 1; }

    } else {

      if (likely(rand_below(afl, 100) < SKIP_NFAV_OLD_PROB)) { return 1; }

    }

  }

#endif                                                     /* ^IGNORE_FINDS */

  if (afl->not_on_tty) {

    ACTF("Fuzzing test case #%u (%u total, %llu uniq crashes found)...",
         afl->current_entry, afl->queued_paths, afl->unique_crashes);
    fflush(stdout);

  }

  /* Map the test case into memory. */
  orig_in = in_buf = queue_testcase_get(afl, afl->queue_cur);
  len = afl->queue_cur->len;

  out_buf = afl_realloc(AFL_BUF_PARAM(out), len);
  if (unlikely(!out_buf)) { PFATAL("alloc"); }

  afl->subseq_tmouts = 0;

  afl->cur_depth = afl->queue_cur->depth;

  /*******************************************
   * CALIBRATION (only if failed earlier on) *
   *******************************************/

  if (unlikely(afl->queue_cur->cal_failed)) {

    u8 res = FSRV_RUN_TMOUT;

    if (afl->queue_cur->cal_failed < CAL_CHANCES) {

      afl->queue_cur->exec_cksum = 0;

      res =
          calibrate_case(afl, afl->queue_cur, in_buf, afl->queue_cycle - 1, 0);

      if (res == FSRV_RUN_ERROR) {

        FATAL("Unable to execute target application");

      }

    }

    if (afl->stop_soon || res != afl->crash_mode) {

      ++afl->cur_skipped_paths;
      goto abandon_entry;

    }

  }

  /************
   * TRIMMING *
   ************/

  if (unlikely(!afl->non_instrumented_mode && !afl->queue_cur->trim_done &&
               !afl->disable_trim)) {

    u32 old_len = afl->queue_cur->len;

    u8 res = trim_case(afl, afl->queue_cur, in_buf);
    orig_in = in_buf = queue_testcase_get(afl, afl->queue_cur);

    if (unlikely(res == FSRV_RUN_ERROR)) {

      FATAL("Unable to execute target application");

    }

    if (unlikely(afl->stop_soon)) {

      ++afl->cur_skipped_paths;
      goto abandon_entry;

    }

    /* Don't retry trimming, even if it failed. */

    afl->queue_cur->trim_done = 1;

    len = afl->queue_cur->len;

    /* maybe current entry is not ready for splicing anymore */
    if (unlikely(len <= 4 && old_len > 4)) --afl->ready_for_splicing_count;

  }

  memcpy(out_buf, in_buf, len);

  /*********************
   * PERFORMANCE SCORE *
   *********************/

  if (likely(!afl->old_seed_selection))
    orig_perf = perf_score = afl->queue_cur->perf_score;
  else
    orig_perf = perf_score = calculate_score(afl, afl->queue_cur);

  if (unlikely(perf_score <= 0)) { goto abandon_entry; }

  if (unlikely(afl->shm.cmplog_mode &&
               afl->queue_cur->colorized < afl->cmplog_lvl &&
               (u32)len <= afl->cmplog_max_filesize)) {

    if (unlikely(len < 4)) {

      afl->queue_cur->colorized = CMPLOG_LVL_MAX;

    } else {

      if (afl->cmplog_lvl == 3 ||
          (afl->cmplog_lvl == 2 && afl->queue_cur->tc_ref) ||
          !(afl->fsrv.total_execs % afl->queued_paths) ||
          get_cur_time() - afl->last_path_time > 300000) {  // 300 seconds

        if (input_to_state_stage(afl, in_buf, out_buf, len)) {

          goto abandon_entry;

        }

      }

    }

  }

  /* Go to pacemker fuzzing if MOpt is doing well */

  cur_ms_lv = get_cur_time();
  if (!(afl->key_puppet == 0 &&
        ((cur_ms_lv - afl->last_path_time < (u32)afl->limit_time_puppet) ||
         (afl->last_crash_time != 0 &&
          cur_ms_lv - afl->last_crash_time < (u32)afl->limit_time_puppet) ||
         afl->last_path_time == 0))) {

    afl->key_puppet = 1;
    goto pacemaker_fuzzing;

  }

  /* Skip right away if -d is given, if we have done deterministic fuzzing on
     this entry ourselves (was_fuzzed), or if it has gone through deterministic
     testing in earlier, resumed runs (passed_det). */

  if (likely(afl->skip_deterministic || afl->queue_cur->was_fuzzed ||
             afl->queue_cur->passed_det)) {

    goto havoc_stage;

  }

  /* Skip deterministic fuzzing if exec path checksum puts this out of scope
     for this main instance. */

  if (unlikely(afl->main_node_max &&
               (afl->queue_cur->exec_cksum % afl->main_node_max) !=
                   afl->main_node_id - 1)) {

    goto havoc_stage;

  }

  doing_det = 1;

  /*********************************************
   * SIMPLE BITFLIP (+dictionary construction) *
   *********************************************/

#define FLIP_BIT(_ar, _b)                   \
  do {                                      \
                                            \
    u8 *_arf = (u8 *)(_ar);                 \
    u32 _bf = (_b);                         \
    _arf[(_bf) >> 3] ^= (128 >> ((_bf)&7)); \
                                            \
  } while (0)

  /* Single walking bit. */

  afl->stage_short = "flip1";
  afl->stage_max = len << 3;
  afl->stage_name = "bitflip 1/1";

  afl->stage_val_type = STAGE_VAL_NONE;

  orig_hit_cnt = afl->queued_paths + afl->unique_crashes;

  prev_cksum = afl->queue_cur->exec_cksum;

  for (afl->stage_cur = 0; afl->stage_cur < afl->stage_max; ++afl->stage_cur) {

    afl->stage_cur_byte = afl->stage_cur >> 3;

    FLIP_BIT(out_buf, afl->stage_cur);

#ifdef INTROSPECTION
    snprintf(afl->mutation, sizeof(afl->mutation), "%s MOPT_FLIP_BIT1-%u",
             afl->queue_cur->fname, afl->stage_cur);
#endif
    if (common_fuzz_stuff(afl, out_buf, len)) { goto abandon_entry; }

    FLIP_BIT(out_buf, afl->stage_cur);

    /* While flipping the least significant bit in every byte, pull of an extra
       trick to detect possible syntax tokens. In essence, the idea is that if
       you have a binary blob like this:

       xxxxxxxxIHDRxxxxxxxx

       ...and changing the leading and trailing bytes causes variable or no
       changes in program flow, but touching any character in the "IHDR" string
       always produces the same, distinctive path, it's highly likely that
       "IHDR" is an atomically-checked magic value of special significance to
       the fuzzed format.

       We do this here, rather than as a separate stage, because it's a nice
       way to keep the operation approximately "free" (i.e., no extra execs).

       Empirically, performing the check when flipping the least significant bit
       is advantageous, compared to doing it at the time of more disruptive
       changes, where the program flow may be affected in more violent ways.

       The caveat is that we won't generate dictionaries in the -d mode or -S
       mode - but that's probably a fair trade-off.

       This won't work particularly well with paths that exhibit variable
       behavior, but fails gracefully, so we'll carry out the checks anyway.

      */

    if (!afl->non_instrumented_mode && (afl->stage_cur & 7) == 7) {

      u64 cksum = hash64(afl->fsrv.trace_bits, afl->fsrv.map_size, HASH_CONST);

      if (afl->stage_cur == afl->stage_max - 1 && cksum == prev_cksum) {

        /* If at end of file and we are still collecting a string, grab the
           final character and force output. */

        if (a_len < MAX_AUTO_EXTRA) {

          a_collect[a_len] = out_buf[afl->stage_cur >> 3];

        }

        ++a_len;

        if (a_len >= MIN_AUTO_EXTRA && a_len <= MAX_AUTO_EXTRA) {

          maybe_add_auto(afl, a_collect, a_len);

        }

      } else if (cksum != prev_cksum) {

        /* Otherwise, if the checksum has changed, see if we have something
           worthwhile queued up, and collect that if the answer is yes. */

        if (a_len >= MIN_AUTO_EXTRA && a_len <= MAX_AUTO_EXTRA) {

          maybe_add_auto(afl, a_collect, a_len);

        }

        a_len = 0;
        prev_cksum = cksum;

      }

      /* Continue collecting string, but only if the bit flip actually made
         any difference - we don't want no-op tokens. */

      if (cksum != afl->queue_cur->exec_cksum) {

        if (a_len < MAX_AUTO_EXTRA) {

          a_collect[a_len] = out_buf[afl->stage_cur >> 3];

        }

        ++a_len;

      }

    }                                       /* if (afl->stage_cur & 7) == 7 */

  }                                                   /* for afl->stage_cur */

  new_hit_cnt = afl->queued_paths + afl->unique_crashes;

  afl->stage_finds[STAGE_FLIP1] += new_hit_cnt - orig_hit_cnt;
  afl->stage_cycles[STAGE_FLIP1] += afl->stage_max;

  /* Two walking bits. */

  afl->stage_name = "bitflip 2/1";
  afl->stage_short = "flip2";
  afl->stage_max = (len << 3) - 1;

  orig_hit_cnt = new_hit_cnt;

  for (afl->stage_cur = 0; afl->stage_cur < afl->stage_max; ++afl->stage_cur) {

    afl->stage_cur_byte = afl->stage_cur >> 3;

    FLIP_BIT(out_buf, afl->stage_cur);
    FLIP_BIT(out_buf, afl->stage_cur + 1);

#ifdef INTROSPECTION
    snprintf(afl->mutation, sizeof(afl->mutation), "%s MOPT_FLIP_BIT2-%u",
             afl->queue_cur->fname, afl->stage_cur);
#endif
    if (common_fuzz_stuff(afl, out_buf, len)) { goto abandon_entry; }

    FLIP_BIT(out_buf, afl->stage_cur);
    FLIP_BIT(out_buf, afl->stage_cur + 1);

  }                                                   /* for afl->stage_cur */

  new_hit_cnt = afl->queued_paths + afl->unique_crashes;

  afl->stage_finds[STAGE_FLIP2] += new_hit_cnt - orig_hit_cnt;
  afl->stage_cycles[STAGE_FLIP2] += afl->stage_max;

  /* Four walking bits. */

  afl->stage_name = "bitflip 4/1";
  afl->stage_short = "flip4";
  afl->stage_max = (len << 3) - 3;

  orig_hit_cnt = new_hit_cnt;

  for (afl->stage_cur = 0; afl->stage_cur < afl->stage_max; ++afl->stage_cur) {

    afl->stage_cur_byte = afl->stage_cur >> 3;

    FLIP_BIT(out_buf, afl->stage_cur);
    FLIP_BIT(out_buf, afl->stage_cur + 1);
    FLIP_BIT(out_buf, afl->stage_cur + 2);
    FLIP_BIT(out_buf, afl->stage_cur + 3);

#ifdef INTROSPECTION
    snprintf(afl->mutation, sizeof(afl->mutation), "%s MOPT_FLIP_BIT4-%u",
             afl->queue_cur->fname, afl->stage_cur);
#endif
    if (common_fuzz_stuff(afl, out_buf, len)) { goto abandon_entry; }

    FLIP_BIT(out_buf, afl->stage_cur);
    FLIP_BIT(out_buf, afl->stage_cur + 1);
    FLIP_BIT(out_buf, afl->stage_cur + 2);
    FLIP_BIT(out_buf, afl->stage_cur + 3);

  }                                                   /* for afl->stage_cur */

  new_hit_cnt = afl->queued_paths + afl->unique_crashes;

  afl->stage_finds[STAGE_FLIP4] += new_hit_cnt - orig_hit_cnt;
  afl->stage_cycles[STAGE_FLIP4] += afl->stage_max;

  /* Effector map setup. These macros calculate:

     EFF_APOS      - position of a particular file offset in the map.
     EFF_ALEN      - length of a map with a particular number of bytes.
     EFF_SPAN_ALEN - map span for a sequence of bytes.

   */

#define EFF_APOS(_p) ((_p) >> EFF_MAP_SCALE2)
#define EFF_REM(_x) ((_x) & ((1 << EFF_MAP_SCALE2) - 1))
#define EFF_ALEN(_l) (EFF_APOS(_l) + !!EFF_REM(_l))
#define EFF_SPAN_ALEN(_p, _l) (EFF_APOS((_p) + (_l)-1) - EFF_APOS(_p) + 1)

  /* Initialize effector map for the next step (see comments below). Always
         flag first and last byte as doing something. */

  eff_map = afl_realloc(AFL_BUF_PARAM(eff), EFF_ALEN(len));
  if (unlikely(!eff_map)) { PFATAL("alloc"); }
  eff_map[0] = 1;

  if (EFF_APOS(len - 1) != 0) {

    eff_map[EFF_APOS(len - 1)] = 1;
    ++eff_cnt;

  }

  /* Walking byte. */

  afl->stage_name = "bitflip 8/8";
  afl->stage_short = "flip8";
  afl->stage_max = len;

  orig_hit_cnt = new_hit_cnt;

  for (afl->stage_cur = 0; afl->stage_cur < afl->stage_max; ++afl->stage_cur) {

    afl->stage_cur_byte = afl->stage_cur;

    out_buf[afl->stage_cur] ^= 0xFF;

#ifdef INTROSPECTION
    snprintf(afl->mutation, sizeof(afl->mutation), "%s MOPT_FLIP_BIT8-%u",
             afl->queue_cur->fname, afl->stage_cur);
#endif
    if (common_fuzz_stuff(afl, out_buf, len)) { goto abandon_entry; }

    /* We also use this stage to pull off a simple trick: we identify
       bytes that seem to have no effect on the current execution path
       even when fully flipped - and we skip them during more expensive
       deterministic stages, such as arithmetics or known ints. */

    if (!eff_map[EFF_APOS(afl->stage_cur)]) {

      u64 cksum;

      /* If in non-instrumented mode or if the file is very short, just flag
         everything without wasting time on checksums. */

      if (!afl->non_instrumented_mode && len >= EFF_MIN_LEN) {

        cksum = hash64(afl->fsrv.trace_bits, afl->fsrv.map_size, HASH_CONST);

      } else {

        cksum = ~afl->queue_cur->exec_cksum;

      }

      if (cksum != afl->queue_cur->exec_cksum) {

        eff_map[EFF_APOS(afl->stage_cur)] = 1;
        ++eff_cnt;

      }

    }

    out_buf[afl->stage_cur] ^= 0xFF;

  }                                                   /* for afl->stage_cur */

  /* If the effector map is more than EFF_MAX_PERC dense, just flag the
     whole thing as worth fuzzing, since we wouldn't be saving much time
     anyway. */

  if (eff_cnt != (u32)EFF_ALEN(len) &&
      eff_cnt * 100 / EFF_ALEN(len) > EFF_MAX_PERC) {

    memset(eff_map, 1, EFF_ALEN(len));

    afl->blocks_eff_select += EFF_ALEN(len);

  } else {

    afl->blocks_eff_select += eff_cnt;

  }

  afl->blocks_eff_total += EFF_ALEN(len);

  new_hit_cnt = afl->queued_paths + afl->unique_crashes;

  afl->stage_finds[STAGE_FLIP8] += new_hit_cnt - orig_hit_cnt;
  afl->stage_cycles[STAGE_FLIP8] += afl->stage_max;

  /* Two walking bytes. */

  if (len < 2) { goto skip_bitflip; }

  afl->stage_name = "bitflip 16/8";
  afl->stage_short = "flip16";
  afl->stage_cur = 0;
  afl->stage_max = len - 1;

  orig_hit_cnt = new_hit_cnt;

  for (i = 0; i < len - 1; ++i) {

    /* Let's consult the effector map... */

    if (!eff_map[EFF_APOS(i)] && !eff_map[EFF_APOS(i + 1)]) {

      --afl->stage_max;
      continue;

    }

    afl->stage_cur_byte = i;

    *(u16 *)(out_buf + i) ^= 0xFFFF;

#ifdef INTROSPECTION
    snprintf(afl->mutation, sizeof(afl->mutation), "%s MOPT_FLIP_BIT16-%u",
             afl->queue_cur->fname, afl->stage_cur);
#endif
    if (common_fuzz_stuff(afl, out_buf, len)) { goto abandon_entry; }
    ++afl->stage_cur;

    *(u16 *)(out_buf + i) ^= 0xFFFF;

  }                                                   /* for i = 0; i < len */

  new_hit_cnt = afl->queued_paths + afl->unique_crashes;

  afl->stage_finds[STAGE_FLIP16] += new_hit_cnt - orig_hit_cnt;
  afl->stage_cycles[STAGE_FLIP16] += afl->stage_max;

  if (len < 4) { goto skip_bitflip; }

  /* Four walking bytes. */

  afl->stage_name = "bitflip 32/8";
  afl->stage_short = "flip32";
  afl->stage_cur = 0;
  afl->stage_max = len - 3;

  orig_hit_cnt = new_hit_cnt;

  for (i = 0; i < len - 3; ++i) {

    /* Let's consult the effector map... */
    if (!eff_map[EFF_APOS(i)] && !eff_map[EFF_APOS(i + 1)] &&
        !eff_map[EFF_APOS(i + 2)] && !eff_map[EFF_APOS(i + 3)]) {

      --afl->stage_max;
      continue;

    }

    afl->stage_cur_byte = i;

    *(u32 *)(out_buf + i) ^= 0xFFFFFFFF;

#ifdef INTROSPECTION
    snprintf(afl->mutation, sizeof(afl->mutation), "%s MOPT_FLIP_BIT32-%u",
             afl->queue_cur->fname, afl->stage_cur);
#endif
    if (common_fuzz_stuff(afl, out_buf, len)) { goto abandon_entry; }
    ++afl->stage_cur;

    *(u32 *)(out_buf + i) ^= 0xFFFFFFFF;

  }                                               /* for i = 0; i < len - 3 */

  new_hit_cnt = afl->queued_paths + afl->unique_crashes;

  afl->stage_finds[STAGE_FLIP32] += new_hit_cnt - orig_hit_cnt;
  afl->stage_cycles[STAGE_FLIP32] += afl->stage_max;

skip_bitflip:

  if (afl->no_arith) { goto skip_arith; }

  /**********************
   * ARITHMETIC INC/DEC *
   **********************/

  /* 8-bit arithmetics. */

  afl->stage_name = "arith 8/8";
  afl->stage_short = "arith8";
  afl->stage_cur = 0;
  afl->stage_max = 2 * len * ARITH_MAX;

  afl->stage_val_type = STAGE_VAL_LE;

  orig_hit_cnt = new_hit_cnt;

  for (i = 0; i < (u32)len; ++i) {

    u8 orig = out_buf[i];

    /* Let's consult the effector map... */

    if (!eff_map[EFF_APOS(i)]) {

      afl->stage_max -= 2 * ARITH_MAX;
      continue;

    }

    afl->stage_cur_byte = i;

    for (j = 1; j <= ARITH_MAX; ++j) {

      u8 r = orig ^ (orig + j);

      /* Do arithmetic operations only if the result couldn't be a product
         of a bitflip. */

      if (!could_be_bitflip(r)) {

        afl->stage_cur_val = j;
        out_buf[i] = orig + j;

#ifdef INTROSPECTION
        snprintf(afl->mutation, sizeof(afl->mutation), "%s MOPT_ARITH8+-%u-%u",
                 afl->queue_cur->fname, i, j);
#endif
        if (common_fuzz_stuff(afl, out_buf, len)) { goto abandon_entry; }
        ++afl->stage_cur;

      } else {

        --afl->stage_max;

      }

      r = orig ^ (orig - j);

      if (!could_be_bitflip(r)) {

        afl->stage_cur_val = -j;
        out_buf[i] = orig - j;

#ifdef INTROSPECTION
        snprintf(afl->mutation, sizeof(afl->mutation), "%s MOPT_ARITH8_-%u-%u",
                 afl->queue_cur->fname, i, j);
#endif
        if (common_fuzz_stuff(afl, out_buf, len)) { goto abandon_entry; }
        ++afl->stage_cur;

      } else {

        --afl->stage_max;

      }

      out_buf[i] = orig;

    }

  }                                                   /* for i = 0; i < len */

  new_hit_cnt = afl->queued_paths + afl->unique_crashes;

  afl->stage_finds[STAGE_ARITH8] += new_hit_cnt - orig_hit_cnt;
  afl->stage_cycles[STAGE_ARITH8] += afl->stage_max;

  /* 16-bit arithmetics, both endians. */

  if (len < 2) { goto skip_arith; }

  afl->stage_name = "arith 16/8";
  afl->stage_short = "arith16";
  afl->stage_cur = 0;
  afl->stage_max = 4 * (len - 1) * ARITH_MAX;

  orig_hit_cnt = new_hit_cnt;

  for (i = 0; i < len - 1; ++i) {

    u16 orig = *(u16 *)(out_buf + i);

    /* Let's consult the effector map... */

    if (!eff_map[EFF_APOS(i)] && !eff_map[EFF_APOS(i + 1)]) {

      afl->stage_max -= 4 * ARITH_MAX;
      continue;

    }

    afl->stage_cur_byte = i;

    for (j = 1; j <= ARITH_MAX; ++j) {

      u16 r1 = orig ^ (orig + j), r2 = orig ^ (orig - j),
          r3 = orig ^ SWAP16(SWAP16(orig) + j),
          r4 = orig ^ SWAP16(SWAP16(orig) - j);

      /* Try little endian addition and subtraction first. Do it only
         if the operation would affect more than one byte (hence the
         & 0xff overflow checks) and if it couldn't be a product of
         a bitflip. */

      afl->stage_val_type = STAGE_VAL_LE;

      if ((orig & 0xff) + j > 0xff && !could_be_bitflip(r1)) {

        afl->stage_cur_val = j;
        *(u16 *)(out_buf + i) = orig + j;

#ifdef INTROSPECTION
        snprintf(afl->mutation, sizeof(afl->mutation), "%s MOPT_ARITH16+-%u-%u",
                 afl->queue_cur->fname, i, j);
#endif
        if (common_fuzz_stuff(afl, out_buf, len)) { goto abandon_entry; }
        ++afl->stage_cur;

      } else {

        --afl->stage_max;

      }

      if ((orig & 0xff) < j && !could_be_bitflip(r2)) {

        afl->stage_cur_val = -j;
        *(u16 *)(out_buf + i) = orig - j;

#ifdef INTROSPECTION
        snprintf(afl->mutation, sizeof(afl->mutation), "%s MOPT_ARITH16_-%u-%u",
                 afl->queue_cur->fname, i, j);
#endif
        if (common_fuzz_stuff(afl, out_buf, len)) { goto abandon_entry; }
        ++afl->stage_cur;

      } else {

        --afl->stage_max;

      }

      /* Big endian comes next. Same deal. */

      afl->stage_val_type = STAGE_VAL_BE;

      if ((orig >> 8) + j > 0xff && !could_be_bitflip(r3)) {

        afl->stage_cur_val = j;
        *(u16 *)(out_buf + i) = SWAP16(SWAP16(orig) + j);

#ifdef INTROSPECTION
        snprintf(afl->mutation, sizeof(afl->mutation),
                 "%s MOPT_ARITH16+BE-%u-%u", afl->queue_cur->fname, i, j);
#endif
        if (common_fuzz_stuff(afl, out_buf, len)) { goto abandon_entry; }
        ++afl->stage_cur;

      } else {

        --afl->stage_max;

      }

      if ((orig >> 8) < j && !could_be_bitflip(r4)) {

        afl->stage_cur_val = -j;
        *(u16 *)(out_buf + i) = SWAP16(SWAP16(orig) - j);

#ifdef INTROSPECTION
        snprintf(afl->mutation, sizeof(afl->mutation),
                 "%s MOPT_ARITH16_BE+%u+%u", afl->queue_cur->fname, i, j);
#endif
        if (common_fuzz_stuff(afl, out_buf, len)) { goto abandon_entry; }
        ++afl->stage_cur;

      } else {

        --afl->stage_max;

      }

      *(u16 *)(out_buf + i) = orig;

    }

  }                                               /* for i = 0; i < len - 1 */

  new_hit_cnt = afl->queued_paths + afl->unique_crashes;

  afl->stage_finds[STAGE_ARITH16] += new_hit_cnt - orig_hit_cnt;
  afl->stage_cycles[STAGE_ARITH16] += afl->stage_max;

  /* 32-bit arithmetics, both endians. */

  if (len < 4) { goto skip_arith; }

  afl->stage_name = "arith 32/8";
  afl->stage_short = "arith32";
  afl->stage_cur = 0;
  afl->stage_max = 4 * (len - 3) * ARITH_MAX;

  orig_hit_cnt = new_hit_cnt;

  for (i = 0; i < len - 3; ++i) {

    u32 orig = *(u32 *)(out_buf + i);

    /* Let's consult the effector map... */

    if (!eff_map[EFF_APOS(i)] && !eff_map[EFF_APOS(i + 1)] &&
        !eff_map[EFF_APOS(i + 2)] && !eff_map[EFF_APOS(i + 3)]) {

      afl->stage_max -= 4 * ARITH_MAX;
      continue;

    }

    afl->stage_cur_byte = i;

    for (j = 1; j <= ARITH_MAX; ++j) {

      u32 r1 = orig ^ (orig + j), r2 = orig ^ (orig - j),
          r3 = orig ^ SWAP32(SWAP32(orig) + j),
          r4 = orig ^ SWAP32(SWAP32(orig) - j);

      /* Little endian first. Same deal as with 16-bit: we only want to
         try if the operation would have effect on more than two bytes. */

      afl->stage_val_type = STAGE_VAL_LE;

      if ((orig & 0xffff) + j > 0xffff && !could_be_bitflip(r1)) {

        afl->stage_cur_val = j;
        *(u32 *)(out_buf + i) = orig + j;

#ifdef INTROSPECTION
        snprintf(afl->mutation, sizeof(afl->mutation), "%s MOPT_ARITH32+-%u-%u",
                 afl->queue_cur->fname, i, j);
#endif
        if (common_fuzz_stuff(afl, out_buf, len)) { goto abandon_entry; }
        ++afl->stage_cur;

      } else {

        --afl->stage_max;

      }

      if ((orig & 0xffff) < j && !could_be_bitflip(r2)) {

        afl->stage_cur_val = -j;
        *(u32 *)(out_buf + i) = orig - j;

#ifdef INTROSPECTION
        snprintf(afl->mutation, sizeof(afl->mutation), "%s MOPT_ARITH32_-%u-%u",
                 afl->queue_cur->fname, i, j);
#endif
        if (common_fuzz_stuff(afl, out_buf, len)) { goto abandon_entry; }
        ++afl->stage_cur;

      } else {

        --afl->stage_max;

      }

      /* Big endian next. */

      afl->stage_val_type = STAGE_VAL_BE;

      if ((SWAP32(orig) & 0xffff) + j > 0xffff && !could_be_bitflip(r3)) {

        afl->stage_cur_val = j;
        *(u32 *)(out_buf + i) = SWAP32(SWAP32(orig) + j);

#ifdef INTROSPECTION
        snprintf(afl->mutation, sizeof(afl->mutation),
                 "%s MOPT_ARITH32+BE-%u-%u", afl->queue_cur->fname, i, j);
#endif
        if (common_fuzz_stuff(afl, out_buf, len)) { goto abandon_entry; }
        ++afl->stage_cur;

      } else {

        --afl->stage_max;

      }

      if ((SWAP32(orig) & 0xffff) < j && !could_be_bitflip(r4)) {

        afl->stage_cur_val = -j;
        *(u32 *)(out_buf + i) = SWAP32(SWAP32(orig) - j);

#ifdef INTROSPECTION
        snprintf(afl->mutation, sizeof(afl->mutation),
                 "%s MOPT_ARITH32_BE-%u-%u", afl->queue_cur->fname, i, j);
#endif
        if (common_fuzz_stuff(afl, out_buf, len)) { goto abandon_entry; }
        ++afl->stage_cur;

      } else {

        --afl->stage_max;

      }

      *(u32 *)(out_buf + i) = orig;

    }

  }                                               /* for i = 0; i < len - 3 */

  new_hit_cnt = afl->queued_paths + afl->unique_crashes;

  afl->stage_finds[STAGE_ARITH32] += new_hit_cnt - orig_hit_cnt;
  afl->stage_cycles[STAGE_ARITH32] += afl->stage_max;

skip_arith:

  /**********************
   * INTERESTING VALUES *
   **********************/

  afl->stage_name = "interest 8/8";
  afl->stage_short = "int8";
  afl->stage_cur = 0;
  afl->stage_max = len * sizeof(interesting_8);

  afl->stage_val_type = STAGE_VAL_LE;

  orig_hit_cnt = new_hit_cnt;

  /* Setting 8-bit integers. */

  for (i = 0; i < (u32)len; ++i) {

    u8 orig = out_buf[i];

    /* Let's consult the effector map... */

    if (!eff_map[EFF_APOS(i)]) {

      afl->stage_max -= sizeof(interesting_8);
      continue;

    }

    afl->stage_cur_byte = i;

    for (j = 0; j < sizeof(interesting_8); ++j) {

      /* Skip if the value could be a product of bitflips or arithmetics. */

      if (could_be_bitflip(orig ^ (u8)interesting_8[j]) ||
          could_be_arith(orig, (u8)interesting_8[j], 1)) {

        --afl->stage_max;
        continue;

      }

      afl->stage_cur_val = interesting_8[j];
      out_buf[i] = interesting_8[j];

#ifdef INTROSPECTION
      snprintf(afl->mutation, sizeof(afl->mutation),
               "%s MOPT_INTERESTING8-%u-%u", afl->queue_cur->fname, i, j);
#endif
      if (common_fuzz_stuff(afl, out_buf, len)) { goto abandon_entry; }

      out_buf[i] = orig;
      ++afl->stage_cur;

    }

  }                                                   /* for i = 0; i < len */

  new_hit_cnt = afl->queued_paths + afl->unique_crashes;

  afl->stage_finds[STAGE_INTEREST8] += new_hit_cnt - orig_hit_cnt;
  afl->stage_cycles[STAGE_INTEREST8] += afl->stage_max;

  /* Setting 16-bit integers, both endians. */

  if (afl->no_arith || len < 2) { goto skip_interest; }

  afl->stage_name = "interest 16/8";
  afl->stage_short = "int16";
  afl->stage_cur = 0;
  afl->stage_max = 2 * (len - 1) * (sizeof(interesting_16) >> 1);

  orig_hit_cnt = new_hit_cnt;

  for (i = 0; i < len - 1; ++i) {

    u16 orig = *(u16 *)(out_buf + i);

    /* Let's consult the effector map... */

    if (!eff_map[EFF_APOS(i)] && !eff_map[EFF_APOS(i + 1)]) {

      afl->stage_max -= sizeof(interesting_16);
      continue;

    }

    afl->stage_cur_byte = i;

    for (j = 0; j < sizeof(interesting_16) / 2; ++j) {

      afl->stage_cur_val = interesting_16[j];

      /* Skip if this could be a product of a bitflip, arithmetics,
         or single-byte interesting value insertion. */

      if (!could_be_bitflip(orig ^ (u16)interesting_16[j]) &&
          !could_be_arith(orig, (u16)interesting_16[j], 2) &&
          !could_be_interest(orig, (u16)interesting_16[j], 2, 0)) {

        afl->stage_val_type = STAGE_VAL_LE;

        *(u16 *)(out_buf + i) = interesting_16[j];

#ifdef INTROSPECTION
        snprintf(afl->mutation, sizeof(afl->mutation),
                 "%s MOPT_INTERESTING16-%u-%u", afl->queue_cur->fname, i, j);
#endif
        if (common_fuzz_stuff(afl, out_buf, len)) { goto abandon_entry; }
        ++afl->stage_cur;

      } else {

        --afl->stage_max;

      }

      if ((u16)interesting_16[j] != SWAP16(interesting_16[j]) &&
          !could_be_bitflip(orig ^ SWAP16(interesting_16[j])) &&
          !could_be_arith(orig, SWAP16(interesting_16[j]), 2) &&
          !could_be_interest(orig, SWAP16(interesting_16[j]), 2, 1)) {

        afl->stage_val_type = STAGE_VAL_BE;

#ifdef INTROSPECTION
        snprintf(afl->mutation, sizeof(afl->mutation),
                 "%s MOPT_INTERESTING16BE-%u-%u", afl->queue_cur->fname, i, j);
#endif
        *(u16 *)(out_buf + i) = SWAP16(interesting_16[j]);
        if (common_fuzz_stuff(afl, out_buf, len)) { goto abandon_entry; }
        ++afl->stage_cur;

      } else {

        --afl->stage_max;

      }

    }

    *(u16 *)(out_buf + i) = orig;

  }                                               /* for i = 0; i < len - 1 */

  new_hit_cnt = afl->queued_paths + afl->unique_crashes;

  afl->stage_finds[STAGE_INTEREST16] += new_hit_cnt - orig_hit_cnt;
  afl->stage_cycles[STAGE_INTEREST16] += afl->stage_max;

  if (len < 4) { goto skip_interest; }

  /* Setting 32-bit integers, both endians. */

  afl->stage_name = "interest 32/8";
  afl->stage_short = "int32";
  afl->stage_cur = 0;
  afl->stage_max = 2 * (len - 3) * (sizeof(interesting_32) >> 2);

  orig_hit_cnt = new_hit_cnt;

  for (i = 0; i < len - 3; ++i) {

    u32 orig = *(u32 *)(out_buf + i);

    /* Let's consult the effector map... */

    if (!eff_map[EFF_APOS(i)] && !eff_map[EFF_APOS(i + 1)] &&
        !eff_map[EFF_APOS(i + 2)] && !eff_map[EFF_APOS(i + 3)]) {

      afl->stage_max -= sizeof(interesting_32) >> 1;
      continue;

    }

    afl->stage_cur_byte = i;

    for (j = 0; j < sizeof(interesting_32) / 4; ++j) {

      afl->stage_cur_val = interesting_32[j];

      /* Skip if this could be a product of a bitflip, arithmetics,
         or word interesting value insertion. */

      if (!could_be_bitflip(orig ^ (u32)interesting_32[j]) &&
          !could_be_arith(orig, interesting_32[j], 4) &&
          !could_be_interest(orig, interesting_32[j], 4, 0)) {

        afl->stage_val_type = STAGE_VAL_LE;

        *(u32 *)(out_buf + i) = interesting_32[j];

#ifdef INTROSPECTION
        snprintf(afl->mutation, sizeof(afl->mutation),
                 "%s MOPT_INTERESTING32-%u-%u", afl->queue_cur->fname, i, j);
#endif
        if (common_fuzz_stuff(afl, out_buf, len)) { goto abandon_entry; }
        ++afl->stage_cur;

      } else {

        --afl->stage_max;

      }

      if ((u32)interesting_32[j] != SWAP32(interesting_32[j]) &&
          !could_be_bitflip(orig ^ SWAP32(interesting_32[j])) &&
          !could_be_arith(orig, SWAP32(interesting_32[j]), 4) &&
          !could_be_interest(orig, SWAP32(interesting_32[j]), 4, 1)) {

        afl->stage_val_type = STAGE_VAL_BE;

#ifdef INTROSPECTION
        snprintf(afl->mutation, sizeof(afl->mutation),
                 "%s MOPT_INTERESTING32BE-%u-%u", afl->queue_cur->fname, i, j);
#endif
        *(u32 *)(out_buf + i) = SWAP32(interesting_32[j]);
        if (common_fuzz_stuff(afl, out_buf, len)) { goto abandon_entry; }
        ++afl->stage_cur;

      } else {

        --afl->stage_max;

      }

    }

    *(u32 *)(out_buf + i) = orig;

  }                                               /* for i = 0; i < len - 3 */

  new_hit_cnt = afl->queued_paths + afl->unique_crashes;

  afl->stage_finds[STAGE_INTEREST32] += new_hit_cnt - orig_hit_cnt;
  afl->stage_cycles[STAGE_INTEREST32] += afl->stage_max;

skip_interest:

  /********************
   * DICTIONARY STUFF *
   ********************/

  if (!afl->extras_cnt) { goto skip_user_extras; }

  /* Overwrite with user-supplied extras. */

  afl->stage_name = "user extras (over)";
  afl->stage_short = "ext_UO";
  afl->stage_cur = 0;
  afl->stage_max = afl->extras_cnt * len;

  afl->stage_val_type = STAGE_VAL_NONE;

  orig_hit_cnt = new_hit_cnt;

  for (i = 0; i < (u32)len; ++i) {

    u32 last_len = 0;

    afl->stage_cur_byte = i;

    /* Extras are sorted by size, from smallest to largest. This means
       that we don't have to worry about restoring the buffer in
       between writes at a particular offset determined by the outer
       loop. */

    for (j = 0; j < afl->extras_cnt; ++j) {

      /* Skip extras probabilistically if afl->extras_cnt > AFL_MAX_DET_EXTRAS.
         Also skip them if there's no room to insert the payload, if the token
         is redundant, or if its entire span has no bytes set in the effector
         map. */

      if ((afl->extras_cnt > afl->max_det_extras &&
           rand_below(afl, afl->extras_cnt) >= afl->max_det_extras) ||
          afl->extras[j].len > len - i ||
          !memcmp(afl->extras[j].data, out_buf + i, afl->extras[j].len) ||
          !memchr(eff_map + EFF_APOS(i), 1,
                  EFF_SPAN_ALEN(i, afl->extras[j].len))) {

        --afl->stage_max;
        continue;

      }

      last_len = afl->extras[j].len;
      memcpy(out_buf + i, afl->extras[j].data, last_len);

#ifdef INTROSPECTION
      snprintf(afl->mutation, sizeof(afl->mutation),
               "%s MOPT_EXTRAS_overwrite-%u-%u", afl->queue_cur->fname, i, j);
#endif

      if (common_fuzz_stuff(afl, out_buf, len)) { goto abandon_entry; }

      ++afl->stage_cur;

    }

    /* Restore all the clobbered memory. */
    memcpy(out_buf + i, in_buf + i, last_len);

  }                                                   /* for i = 0; i < len */

  new_hit_cnt = afl->queued_paths + afl->unique_crashes;

  afl->stage_finds[STAGE_EXTRAS_UO] += new_hit_cnt - orig_hit_cnt;
  afl->stage_cycles[STAGE_EXTRAS_UO] += afl->stage_max;

  /* Insertion of user-supplied extras. */

  afl->stage_name = "user extras (insert)";
  afl->stage_short = "ext_UI";
  afl->stage_cur = 0;
  afl->stage_max = afl->extras_cnt * (len + 1);

  orig_hit_cnt = new_hit_cnt;

  ex_tmp = afl_realloc(AFL_BUF_PARAM(ex), len + MAX_DICT_FILE);
  if (unlikely(!ex_tmp)) { PFATAL("alloc"); }

  for (i = 0; i <= (u32)len; ++i) {

    afl->stage_cur_byte = i;

    for (j = 0; j < afl->extras_cnt; ++j) {

      if (len + afl->extras[j].len > MAX_FILE) {

        --afl->stage_max;
        continue;

      }

      /* Insert token */
      memcpy(ex_tmp + i, afl->extras[j].data, afl->extras[j].len);

      /* Copy tail */
      memcpy(ex_tmp + i + afl->extras[j].len, out_buf + i, len - i);

#ifdef INTROSPECTION
      snprintf(afl->mutation, sizeof(afl->mutation),
               "%s MOPT_EXTRAS_insert-%u-%u", afl->queue_cur->fname, i, j);
#endif

      if (common_fuzz_stuff(afl, ex_tmp, len + afl->extras[j].len)) {

        goto abandon_entry;

      }

      ++afl->stage_cur;

    }

    /* Copy head */
    ex_tmp[i] = out_buf[i];

  }                                                  /* for i = 0; i <= len */

  new_hit_cnt = afl->queued_paths + afl->unique_crashes;

  afl->stage_finds[STAGE_EXTRAS_UI] += new_hit_cnt - orig_hit_cnt;
  afl->stage_cycles[STAGE_EXTRAS_UI] += afl->stage_max;

skip_user_extras:

  if (!afl->a_extras_cnt) { goto skip_extras; }

  afl->stage_name = "auto extras (over)";
  afl->stage_short = "ext_AO";
  afl->stage_cur = 0;
  afl->stage_max = MIN(afl->a_extras_cnt, (u32)USE_AUTO_EXTRAS) * len;

  afl->stage_val_type = STAGE_VAL_NONE;

  orig_hit_cnt = new_hit_cnt;

  for (i = 0; i < (u32)len; ++i) {

    u32 last_len = 0;

    afl->stage_cur_byte = i;

    u32 min_extra_len = MIN(afl->a_extras_cnt, (u32)USE_AUTO_EXTRAS);
    for (j = 0; j < min_extra_len; ++j) {

      /* See the comment in the earlier code; extras are sorted by size. */

      if ((afl->a_extras[j].len) > (len - i) ||
          !memcmp(afl->a_extras[j].data, out_buf + i, afl->a_extras[j].len) ||
          !memchr(eff_map + EFF_APOS(i), 1,
                  EFF_SPAN_ALEN(i, afl->a_extras[j].len))) {

        --afl->stage_max;
        continue;

      }

      last_len = afl->a_extras[j].len;
      memcpy(out_buf + i, afl->a_extras[j].data, last_len);

#ifdef INTROSPECTION
      snprintf(afl->mutation, sizeof(afl->mutation),
               "%s MOPT_AUTO_EXTRAS_overwrite-%u-%u", afl->queue_cur->fname, i,
               j);
#endif

      if (common_fuzz_stuff(afl, out_buf, len)) { goto abandon_entry; }

      ++afl->stage_cur;

    }

    /* Restore all the clobbered memory. */
    memcpy(out_buf + i, in_buf + i, last_len);

  }                                                   /* for i = 0; i < len */

  new_hit_cnt = afl->queued_paths + afl->unique_crashes;

  afl->stage_finds[STAGE_EXTRAS_AO] += new_hit_cnt - orig_hit_cnt;
  afl->stage_cycles[STAGE_EXTRAS_AO] += afl->stage_max;

skip_extras:

  /* If we made this to here without jumping to havoc_stage or abandon_entry,
     we're properly done with deterministic steps and can mark it as such
     in the .state/ directory. */

  if (!afl->queue_cur->passed_det) { mark_as_det_done(afl, afl->queue_cur); }

  /****************
   * RANDOM HAVOC *
   ****************/

havoc_stage:
pacemaker_fuzzing:

  afl->stage_cur_byte = -1;

  /* The havoc stage mutation code is also invoked when splicing files; if the
     splice_cycle variable is set, generate different descriptions and such. */

  if (!splice_cycle) {

    afl->stage_name = MOpt_globals.havoc_stagename;
    afl->stage_short = MOpt_globals.havoc_stagenameshort;
    afl->stage_max = (doing_det ? HAVOC_CYCLES_INIT : HAVOC_CYCLES) *
                     perf_score / afl->havoc_div / 100;

  } else {

    perf_score = orig_perf;

    snprintf(afl->stage_name_buf, STAGE_BUF_SIZE,
             MOpt_globals.splice_stageformat, splice_cycle);
    afl->stage_name = afl->stage_name_buf;
    afl->stage_short = MOpt_globals.splice_stagenameshort;
    afl->stage_max = SPLICE_HAVOC * perf_score / afl->havoc_div / 100;

  }

  s32 temp_len_puppet;

  // for (; afl->swarm_now < swarm_num; ++afl->swarm_now)
  {

    if (afl->key_puppet == 1) {

      if (unlikely(afl->orig_hit_cnt_puppet == 0)) {

        afl->orig_hit_cnt_puppet = afl->queued_paths + afl->unique_crashes;
        afl->last_limit_time_start = get_cur_time();
        afl->SPLICE_CYCLES_puppet =
            (rand_below(
                 afl, SPLICE_CYCLES_puppet_up - SPLICE_CYCLES_puppet_low + 1) +
             SPLICE_CYCLES_puppet_low);

      }

    }                                            /* if afl->key_puppet == 1 */

    {

#ifndef IGNORE_FINDS
    havoc_stage_puppet:
#endif

      afl->stage_cur_byte = -1;

      /* The havoc stage mutation code is also invoked when splicing files; if
         the splice_cycle variable is set, generate different descriptions and
         such. */

      if (!splice_cycle) {

        afl->stage_name = MOpt_globals.havoc_stagename;
        afl->stage_short = MOpt_globals.havoc_stagenameshort;
        afl->stage_max = (doing_det ? HAVOC_CYCLES_INIT : HAVOC_CYCLES) *
                         perf_score / afl->havoc_div / 100;

      } else {

        perf_score = orig_perf;
        snprintf(afl->stage_name_buf, STAGE_BUF_SIZE,
                 MOpt_globals.splice_stageformat, splice_cycle);
        afl->stage_name = afl->stage_name_buf;
        afl->stage_short = MOpt_globals.splice_stagenameshort;
        afl->stage_max = SPLICE_HAVOC * perf_score / afl->havoc_div / 100;

      }

      if (afl->stage_max < HAVOC_MIN) { afl->stage_max = HAVOC_MIN; }

      temp_len = len;
      position_map = ck_realloc(position_map, sizeof (u32) * (temp_len + 1));

      orig_hit_cnt = afl->queued_paths + afl->unique_crashes;

      havoc_queued = afl->queued_paths;

      u32 r_max;

      r_max = 15 + ((afl->extras_cnt + afl->a_extras_cnt) ? 2 : 0);

      if (unlikely(afl->expand_havoc && afl->ready_for_splicing_count > 1)) {

        /* add expensive havoc cases here, they are activated after a full
           cycle without finds happened */

        ++r_max;

      }

      for (afl->stage_cur = 0; afl->stage_cur < afl->stage_max;
           ++afl->stage_cur) {

        u32 use_stacking = 1 << (1 + rand_below(afl, afl->havoc_stack_pow2));

        afl->stage_cur_val = use_stacking;

        for (i = 0; i < operator_num; ++i) {

          MOpt_globals.cycles_v3[i] = MOpt_globals.cycles_v2[i];

        }

#ifdef INTROSPECTION
        snprintf(afl->mutation, sizeof(afl->mutation), "%s MOPT_HAVOC-%u",
                 afl->queue_cur->fname, use_stacking);
#endif

        for (i = 0; i < use_stacking; ++i) {

          switch (select_algorithm(afl, r_max)) {

            case 0:
              /* Flip a single bit somewhere. Spooky! */
              FLIP_BIT(out_buf, rand_below(afl, temp_len << 3));
              MOpt_globals.cycles_v2[STAGE_FLIP1]++;
#ifdef INTROSPECTION
              snprintf(afl->m_tmp, sizeof(afl->m_tmp), " FLIP_BIT1");
              strcat(afl->mutation, afl->m_tmp);
#endif
              break;

            case 1:
              if (temp_len < 2) { break; }
              temp_len_puppet = rand_below(afl, (temp_len << 3) - 1);
              FLIP_BIT(out_buf, temp_len_puppet);
              FLIP_BIT(out_buf, temp_len_puppet + 1);
              MOpt_globals.cycles_v2[STAGE_FLIP2]++;
#ifdef INTROSPECTION
              snprintf(afl->m_tmp, sizeof(afl->m_tmp), " FLIP_BIT2");
              strcat(afl->mutation, afl->m_tmp);
#endif
              break;

            case 2:
              if (temp_len < 2) { break; }
              temp_len_puppet = rand_below(afl, (temp_len << 3) - 3);
              FLIP_BIT(out_buf, temp_len_puppet);
              FLIP_BIT(out_buf, temp_len_puppet + 1);
              FLIP_BIT(out_buf, temp_len_puppet + 2);
              FLIP_BIT(out_buf, temp_len_puppet + 3);
              MOpt_globals.cycles_v2[STAGE_FLIP4]++;
#ifdef INTROSPECTION
              snprintf(afl->m_tmp, sizeof(afl->m_tmp), " FLIP_BIT4");
              strcat(afl->mutation, afl->m_tmp);
#endif
              break;

            case 3:
              if (temp_len < 4) { break; }
              out_buf[rand_below(afl, temp_len)] ^= 0xFF;
              MOpt_globals.cycles_v2[STAGE_FLIP8]++;
#ifdef INTROSPECTION
              snprintf(afl->m_tmp, sizeof(afl->m_tmp), " FLIP_BIT8");
              strcat(afl->mutation, afl->m_tmp);
#endif
              break;

            case 4:
              if (temp_len < 8) { break; }
              *(u16 *)(out_buf + rand_below(afl, temp_len - 1)) ^= 0xFFFF;
              MOpt_globals.cycles_v2[STAGE_FLIP16]++;
#ifdef INTROSPECTION
              snprintf(afl->m_tmp, sizeof(afl->m_tmp), " FLIP_BIT16");
              strcat(afl->mutation, afl->m_tmp);
#endif
              break;

            case 5:
              if (temp_len < 8) { break; }
              *(u32 *)(out_buf + rand_below(afl, temp_len - 3)) ^= 0xFFFFFFFF;
              MOpt_globals.cycles_v2[STAGE_FLIP32]++;
#ifdef INTROSPECTION
              snprintf(afl->m_tmp, sizeof(afl->m_tmp), " FLIP_BIT32");
              strcat(afl->mutation, afl->m_tmp);
#endif
              break;

            case 6:
              out_buf[rand_below(afl, temp_len)] -=
                  1 + rand_below(afl, ARITH_MAX);
              out_buf[rand_below(afl, temp_len)] +=
                  1 + rand_below(afl, ARITH_MAX);
              MOpt_globals.cycles_v2[STAGE_ARITH8]++;
#ifdef INTROSPECTION
              snprintf(afl->m_tmp, sizeof(afl->m_tmp), " ARITH8");
              strcat(afl->mutation, afl->m_tmp);
#endif
              break;

            case 7:
              /* Randomly subtract from word, random endian. */
              if (temp_len < 8) { break; }
              if (rand_below(afl, 2)) {

                u32 pos = rand_below(afl, temp_len - 1);
                *(u16 *)(out_buf + pos) -= 1 + rand_below(afl, ARITH_MAX);
#ifdef INTROSPECTION
                snprintf(afl->m_tmp, sizeof(afl->m_tmp), " ARITH16-%u", pos);
                strcat(afl->mutation, afl->m_tmp);
#endif

              } else {

                u32 pos = rand_below(afl, temp_len - 1);
                u16 num = 1 + rand_below(afl, ARITH_MAX);
#ifdef INTROSPECTION
                snprintf(afl->m_tmp, sizeof(afl->m_tmp), " ARITH16BE-%u-%u",
                         pos, num);
                strcat(afl->mutation, afl->m_tmp);
#endif
                *(u16 *)(out_buf + pos) =
                    SWAP16(SWAP16(*(u16 *)(out_buf + pos)) - num);

              }

              /* Randomly add to word, random endian. */
              if (rand_below(afl, 2)) {

                u32 pos = rand_below(afl, temp_len - 1);
#ifdef INTROSPECTION
                snprintf(afl->m_tmp, sizeof(afl->m_tmp), " ARITH16+-%u", pos);
                strcat(afl->mutation, afl->m_tmp);
#endif
                *(u16 *)(out_buf + pos) += 1 + rand_below(afl, ARITH_MAX);

              } else {

                u32 pos = rand_below(afl, temp_len - 1);
                u16 num = 1 + rand_below(afl, ARITH_MAX);
#ifdef INTROSPECTION
                snprintf(afl->m_tmp, sizeof(afl->m_tmp), " ARITH16BE+-%u-%u",
                         pos, num);
                strcat(afl->mutation, afl->m_tmp);
#endif
                *(u16 *)(out_buf + pos) =
                    SWAP16(SWAP16(*(u16 *)(out_buf + pos)) + num);

              }

              MOpt_globals.cycles_v2[STAGE_ARITH16]++;
              break;

            case 8:
              /* Randomly subtract from dword, random endian. */
              if (temp_len < 8) { break; }
              if (rand_below(afl, 2)) {

                u32 pos = rand_below(afl, temp_len - 3);
#ifdef INTROSPECTION
                snprintf(afl->m_tmp, sizeof(afl->m_tmp), " ARITH32_-%u", pos);
                strcat(afl->mutation, afl->m_tmp);
#endif
                *(u32 *)(out_buf + pos) -= 1 + rand_below(afl, ARITH_MAX);

              } else {

                u32 pos = rand_below(afl, temp_len - 3);
                u32 num = 1 + rand_below(afl, ARITH_MAX);
#ifdef INTROSPECTION
                snprintf(afl->m_tmp, sizeof(afl->m_tmp), " ARITH32BE_-%u-%u",
                         pos, num);
                strcat(afl->mutation, afl->m_tmp);
#endif
                *(u32 *)(out_buf + pos) =
                    SWAP32(SWAP32(*(u32 *)(out_buf + pos)) - num);

              }

              /* Randomly add to dword, random endian. */
              // if (temp_len < 4) break;
              if (rand_below(afl, 2)) {

                u32 pos = rand_below(afl, temp_len - 3);
#ifdef INTROSPECTION
                snprintf(afl->m_tmp, sizeof(afl->m_tmp), " ARITH32+-%u", pos);
                strcat(afl->mutation, afl->m_tmp);
#endif
                *(u32 *)(out_buf + pos) += 1 + rand_below(afl, ARITH_MAX);

              } else {

                u32 pos = rand_below(afl, temp_len - 3);
                u32 num = 1 + rand_below(afl, ARITH_MAX);
#ifdef INTROSPECTION
                snprintf(afl->m_tmp, sizeof(afl->m_tmp), " ARITH32BE+-%u-%u",
                         pos, num);
                strcat(afl->mutation, afl->m_tmp);
#endif
                *(u32 *)(out_buf + pos) =
                    SWAP32(SWAP32(*(u32 *)(out_buf + pos)) + num);

              }

              MOpt_globals.cycles_v2[STAGE_ARITH32]++;
              break;

            case 9:
              /* Set byte to interesting value. */
              if (temp_len < 4) { break; }
              out_buf[rand_below(afl, temp_len)] =
                  interesting_8[rand_below(afl, sizeof(interesting_8))];
              MOpt_globals.cycles_v2[STAGE_INTEREST8]++;
#ifdef INTROSPECTION
              snprintf(afl->m_tmp, sizeof(afl->m_tmp), " INTERESTING8");
              strcat(afl->mutation, afl->m_tmp);
#endif
              break;

            case 10:
              /* Set word to interesting value, randomly choosing endian. */
              if (temp_len < 8) { break; }
              if (rand_below(afl, 2)) {

#ifdef INTROSPECTION
                snprintf(afl->m_tmp, sizeof(afl->m_tmp), " INTERESTING16");
                strcat(afl->mutation, afl->m_tmp);
#endif
                *(u16 *)(out_buf + rand_below(afl, temp_len - 1)) =
                    interesting_16[rand_below(afl,
                                              sizeof(interesting_16) >> 1)];

              } else {

#ifdef INTROSPECTION
                snprintf(afl->m_tmp, sizeof(afl->m_tmp), " INTERESTING16BE");
                strcat(afl->mutation, afl->m_tmp);
#endif
                *(u16 *)(out_buf + rand_below(afl, temp_len - 1)) =
                    SWAP16(interesting_16[rand_below(
                        afl, sizeof(interesting_16) >> 1)]);

              }

              MOpt_globals.cycles_v2[STAGE_INTEREST16]++;
              break;

            case 11:
              /* Set dword to interesting value, randomly choosing endian. */

              if (temp_len < 8) { break; }

              if (rand_below(afl, 2)) {

#ifdef INTROSPECTION
                snprintf(afl->m_tmp, sizeof(afl->m_tmp), " INTERESTING32");
                strcat(afl->mutation, afl->m_tmp);
#endif
                *(u32 *)(out_buf + rand_below(afl, temp_len - 3)) =
                    interesting_32[rand_below(afl,
                                              sizeof(interesting_32) >> 2)];

              } else {

#ifdef INTROSPECTION
                snprintf(afl->m_tmp, sizeof(afl->m_tmp), " INTERESTING32BE");
                strcat(afl->mutation, afl->m_tmp);
#endif
                *(u32 *)(out_buf + rand_below(afl, temp_len - 3)) =
                    SWAP32(interesting_32[rand_below(
                        afl, sizeof(interesting_32) >> 2)]);

              }

              MOpt_globals.cycles_v2[STAGE_INTEREST32]++;
              break;

            case 12:

              /* Just set a random byte to a random value. Because,
                 why not. We use XOR with 1-255 to eliminate the
                 possibility of a no-op. */

              out_buf[rand_below(afl, temp_len)] ^= 1 + rand_below(afl, 255);
              MOpt_globals.cycles_v2[STAGE_RANDOMBYTE]++;
#ifdef INTROSPECTION
              snprintf(afl->m_tmp, sizeof(afl->m_tmp), " RAND8");
              strcat(afl->mutation, afl->m_tmp);
#endif
              break;

            case 13: {

              /* Delete bytes. We're making this a bit more likely
                 than insertion (the next option) in hopes of keeping
                 files reasonably small. */

              u32 del_from, del_len;

              if (temp_len < 2) { break; }

              /* Don't delete too much. */

              del_len = choose_block_len(afl, temp_len - 1);

              del_from = rand_below(afl, temp_len - del_len + 1);

#ifdef INTROSPECTION
              snprintf(afl->m_tmp, sizeof(afl->m_tmp), " DEL-%u%u", del_from,
                       del_len);
              strcat(afl->mutation, afl->m_tmp);
#endif
              memmove(out_buf + del_from, out_buf + del_from + del_len,
                      temp_len - del_from - del_len);

              temp_len -= del_len;
              MOpt_globals.cycles_v2[STAGE_DELETEBYTE]++;
              break;

            }

            case 14:

              if (temp_len + HAVOC_BLK_XL < MAX_FILE) {

                /* Clone bytes (75%) or insert a block of constant bytes (25%).
                 */

                u8  actually_clone = rand_below(afl, 4);
                u32 clone_from, clone_to, clone_len;
                u8 *new_buf;

                if (likely(actually_clone)) {

                  clone_len = choose_block_len(afl, temp_len);
                  clone_from = rand_below(afl, temp_len - clone_len + 1);

                } else {

                  clone_len = choose_block_len(afl, HAVOC_BLK_XL);
                  clone_from = 0;

                }

                clone_to = rand_below(afl, temp_len);

#ifdef INTROSPECTION
                snprintf(afl->m_tmp, sizeof(afl->m_tmp), " CLONE_%s-%u-%u-%u",
                         actually_clone ? "clone" : "insert", clone_from,
                         clone_to, clone_len);
                strcat(afl->mutation, afl->m_tmp);
#endif
                new_buf = afl_realloc(AFL_BUF_PARAM(out_scratch),
                                      temp_len + clone_len);
                if (unlikely(!new_buf)) { PFATAL("alloc"); }

                /* Head */

                memcpy(new_buf, out_buf, clone_to);

                /* Inserted part */

                if (actually_clone) {

                  memcpy(new_buf + clone_to, out_buf + clone_from, clone_len);

                } else {

                  memset(new_buf + clone_to,
                         rand_below(afl, 2)
                             ? rand_below(afl, 256)
                             : out_buf[rand_below(afl, temp_len)],
                         clone_len);

                }

                /* Tail */
                memcpy(new_buf + clone_to + clone_len, out_buf + clone_to,
                       temp_len - clone_to);

                out_buf = new_buf;
                afl_swap_bufs(AFL_BUF_PARAM(out), AFL_BUF_PARAM(out_scratch));
                temp_len += clone_len;
                position_map = ck_realloc(position_map, sizeof (u32) * (temp_len + 1));
                MOpt_globals.cycles_v2[STAGE_Clone75]++;

              }

              break;

            case 15: {

              /* Overwrite bytes with a randomly selected chunk (75%) or fixed
                 bytes (25%). */

              u32 copy_from, copy_to, copy_len;

              if (temp_len < 2) { break; }

              copy_len = choose_block_len(afl, temp_len - 1);

              copy_from = rand_below(afl, temp_len - copy_len + 1);
              copy_to = rand_below(afl, temp_len - copy_len + 1);

              if (likely(rand_below(afl, 4))) {

                if (likely(copy_from != copy_to)) {

#ifdef INTROSPECTION
                  snprintf(afl->m_tmp, sizeof(afl->m_tmp),
                           " OVERWRITE_COPY-%u-%u-%u", copy_from, copy_to,
                           copy_len);
                  strcat(afl->mutation, afl->m_tmp);
#endif
                  memmove(out_buf + copy_to, out_buf + copy_from, copy_len);

                }

              } else {

#ifdef INTROSPECTION
                snprintf(afl->m_tmp, sizeof(afl->m_tmp),
                         " OVERWRITE_FIXED-%u-%u-%u", copy_from, copy_to,
                         copy_len);
                strcat(afl->mutation, afl->m_tmp);
#endif
                memset(out_buf + copy_to,
                       rand_below(afl, 2) ? rand_below(afl, 256)
                                          : out_buf[rand_below(afl, temp_len)],
                       copy_len);

              }

              MOpt_globals.cycles_v2[STAGE_OverWrite75]++;
              break;

            }                                                    /* case 15 */

              /* Values 16 and 17 can be selected only if there are any extras
                 present in the dictionaries. */

            case 16: {

              /* Overwrite bytes with an extra. */

              if (!afl->extras_cnt ||
                  (afl->a_extras_cnt && rand_below(afl, 2))) {

                /* No user-specified extras or odds in our favor. Let's use an
                  auto-detected one. */

                u32 use_extra = rand_below(afl, afl->a_extras_cnt);
                u32 extra_len = afl->a_extras[use_extra].len;

                if (extra_len > (u32)temp_len) break;

                u32 insert_at = rand_below(afl, temp_len - extra_len + 1);
#ifdef INTROSPECTION
                snprintf(afl->m_tmp, sizeof(afl->m_tmp),
                         " AUTO_EXTRA_OVERWRITE-%u-%u", insert_at, extra_len);
                strcat(afl->mutation, afl->m_tmp);
#endif
                memcpy(out_buf + insert_at, afl->a_extras[use_extra].data,
                       extra_len);

              } else {

                /* No auto extras or odds in our favor. Use the dictionary. */

                u32 use_extra = rand_below(afl, afl->extras_cnt);
                u32 extra_len = afl->extras[use_extra].len;

                if (extra_len > (u32)temp_len) break;

                u32 insert_at = rand_below(afl, temp_len - extra_len + 1);
#ifdef INTROSPECTION
                snprintf(afl->m_tmp, sizeof(afl->m_tmp),
                         " EXTRA_OVERWRITE-%u-%u", insert_at, extra_len);
                strcat(afl->mutation, afl->m_tmp);
#endif
                memcpy(out_buf + insert_at, afl->extras[use_extra].data,
                       extra_len);

              }

              MOpt_globals.cycles_v2[STAGE_OverWriteExtra]++;

              break;

            }

              /* Insert an extra. */

            case 17: {

              u32 use_extra, extra_len,
                  insert_at = rand_below(afl, temp_len + 1);
              u8 *ptr;

              /* Insert an extra. Do the same dice-rolling stuff as for the
                previous case. */

              if (!afl->extras_cnt ||
                  (afl->a_extras_cnt && rand_below(afl, 2))) {

                use_extra = rand_below(afl, afl->a_extras_cnt);
                extra_len = afl->a_extras[use_extra].len;
                ptr = afl->a_extras[use_extra].data;
#ifdef INTROSPECTION
                snprintf(afl->m_tmp, sizeof(afl->m_tmp),
                         " AUTO_EXTRA_INSERT-%u-%u", insert_at, extra_len);
                strcat(afl->mutation, afl->m_tmp);
#endif

              } else {

                use_extra = rand_below(afl, afl->extras_cnt);
                extra_len = afl->extras[use_extra].len;
                ptr = afl->extras[use_extra].data;
#ifdef INTROSPECTION
                snprintf(afl->m_tmp, sizeof(afl->m_tmp), " EXTRA_INSERT-%u-%u",
                         insert_at, extra_len);
                strcat(afl->mutation, afl->m_tmp);
#endif

              }

              if (temp_len + extra_len >= MAX_FILE) break;

              out_buf = afl_realloc(AFL_BUF_PARAM(out), temp_len + extra_len);
              if (unlikely(!out_buf)) { PFATAL("alloc"); }

              /* Tail */
              memmove(out_buf + insert_at + extra_len, out_buf + insert_at,
                      temp_len - insert_at);

              /* Inserted part */
              memcpy(out_buf + insert_at, ptr, extra_len);

              temp_len += extra_len;
              position_map = ck_realloc(position_map, sizeof (u32) * (temp_len + 1));
              MOpt_globals.cycles_v2[STAGE_InsertExtra]++;
              break;

            }

            default: {

              if (unlikely(afl->ready_for_splicing_count < 2)) break;

              u32 tid;
              do {

                tid = rand_below(afl, afl->queued_paths);

              } while (tid == afl->current_entry ||

                       afl->queue_buf[tid]->len < 4);

              /* Get the testcase for splicing. */
              struct queue_entry *target = afl->queue_buf[tid];
              u32                 new_len = target->len;
              u8 *                new_buf = queue_testcase_get(afl, target);

              if ((temp_len >= 2 && rand_below(afl, 2)) ||
                  temp_len + HAVOC_BLK_XL >= MAX_FILE) {

                /* overwrite mode */

                u32 copy_from, copy_to, copy_len;

                copy_len = choose_block_len(afl, new_len - 1);
                if (copy_len > temp_len) copy_len = temp_len;

                copy_from = rand_below(afl, new_len - copy_len + 1);
                copy_to = rand_below(afl, temp_len - copy_len + 1);

#ifdef INTROSPECTION
                snprintf(afl->m_tmp, sizeof(afl->m_tmp),
                         " SPLICE_OVERWRITE-%u-%u-%u-%s", copy_from, copy_to,
                         copy_len, target->fname);
                strcat(afl->mutation, afl->m_tmp);
#endif
                memmove(out_buf + copy_to, new_buf + copy_from, copy_len);

              } else {

                /* insert mode */

                u32 clone_from, clone_to, clone_len;

                clone_len = choose_block_len(afl, new_len);
                clone_from = rand_below(afl, new_len - clone_len + 1);
                clone_to = rand_below(afl, temp_len + 1);

                u8 *temp_buf = afl_realloc(AFL_BUF_PARAM(out_scratch),
                                           temp_len + clone_len + 1);
                if (unlikely(!temp_buf)) { PFATAL("alloc"); }

#ifdef INTROSPECTION
                snprintf(afl->m_tmp, sizeof(afl->m_tmp),
                         " SPLICE_INSERT-%u-%u-%u-%s", clone_from, clone_to,
                         clone_len, target->fname);
                strcat(afl->mutation, afl->m_tmp);
#endif
                /* Head */

                memcpy(temp_buf, out_buf, clone_to);

                /* Inserted part */

                memcpy(temp_buf + clone_to, new_buf + clone_from, clone_len);

                /* Tail */
                memcpy(temp_buf + clone_to + clone_len, out_buf + clone_to,
                       temp_len - clone_to);

                out_buf = temp_buf;
                afl_swap_bufs(AFL_BUF_PARAM(out), AFL_BUF_PARAM(out_scratch));
                temp_len += clone_len;
                position_map = ck_realloc(position_map, sizeof (u32) * (temp_len + 1));

              }

              MOpt_globals.cycles_v2[STAGE_Splice]++;
              break;

            }  // end of default:

          }                                    /* switch select_algorithm() */

        }                                      /* for i=0; i < use_stacking */

        ++*MOpt_globals.pTime;

        u64 temp_total_found = afl->queued_paths + afl->unique_crashes;

        if (common_fuzz_stuff(afl, out_buf, temp_len)) {

          goto abandon_entry_puppet;

        }

        /* out_buf might have been mangled a bit, so let's restore it to its
           original size and shape. */

        out_buf = afl_realloc(AFL_BUF_PARAM(out), len);
        if (unlikely(!out_buf)) { PFATAL("alloc"); }
        temp_len = len;
        position_map = ck_realloc(position_map, sizeof (u32) * (temp_len + 1));
        memcpy(out_buf, in_buf, len);

        /* If we're finding new stuff, let's run for a bit longer, limits
           permitting. */

        if (afl->queued_paths != havoc_queued) {

          if (perf_score <= afl->havoc_max_mult * 100) {

            afl->stage_max *= 2;
            perf_score *= 2;

          }

          havoc_queued = afl->queued_paths;

        }

        if (unlikely(afl->queued_paths + afl->unique_crashes >
                     temp_total_found)) {

          u64 temp_temp_puppet =
              afl->queued_paths + afl->unique_crashes - temp_total_found;
          afl->total_puppet_find = afl->total_puppet_find + temp_temp_puppet;

          if (MOpt_globals.is_pilot_mode) {

            for (i = 0; i < operator_num; ++i) {

              if (MOpt_globals.cycles_v2[i] > MOpt_globals.cycles_v3[i]) {

                MOpt_globals.finds_v2[i] += temp_temp_puppet;

              }

            }

          } else {

            for (i = 0; i < operator_num; i++) {

              if (afl->core_operator_cycles_puppet_v2[i] >
                  afl->core_operator_cycles_puppet_v3[i])

                afl->core_operator_finds_puppet_v2[i] += temp_temp_puppet;

            }

          }

        }                                                             /* if */

      } /* for (afl->stage_cur = 0; afl->stage_cur < afl->stage_max;

           ++afl->stage_cur) { */

      new_hit_cnt = afl->queued_paths + afl->unique_crashes;

      if (MOpt_globals.is_pilot_mode) {

        if (!splice_cycle) {

          afl->stage_finds[STAGE_HAVOC] += new_hit_cnt - orig_hit_cnt;
          afl->stage_cycles[STAGE_HAVOC] += afl->stage_max;

        } else {

          afl->stage_finds[STAGE_SPLICE] += new_hit_cnt - orig_hit_cnt;
          afl->stage_cycles[STAGE_SPLICE] += afl->stage_max;

        }

      }

#ifndef IGNORE_FINDS

      /************
       * SPLICING *
       ************/

    retry_splicing_puppet:

      if (afl->use_splicing &&
          splice_cycle++ < (u32)afl->SPLICE_CYCLES_puppet &&
          afl->ready_for_splicing_count > 1 && afl->queue_cur->len >= 4) {

        struct queue_entry *target;
        u32                 tid, split_at;
        u8 *                new_buf;
        s32                 f_diff, l_diff;

        /* First of all, if we've modified in_buf for havoc, let's clean that
           up... */

        if (in_buf != orig_in) {

          in_buf = orig_in;
          len = afl->queue_cur->len;

        }

        /* Pick a random queue entry and seek to it. Don't splice with yourself.
         */

        do {

          tid = rand_below(afl, afl->queued_paths);

        } while (tid == afl->current_entry || afl->queue_buf[tid]->len < 4);

        afl->splicing_with = tid;
        target = afl->queue_buf[tid];

        /* Read the testcase into a new buffer. */
        new_buf = queue_testcase_get(afl, target);

        /* Find a suitable splicin g location, somewhere between the first and
           the last differing byte. Bail out if the difference is just a single
           byte or so. */

        locate_diffs(in_buf, new_buf, MIN(len, target->len), &f_diff, &l_diff);

        if (f_diff < 0 || l_diff < 2 || f_diff == l_diff) {

          goto retry_splicing_puppet;

        }

        /* Split somewhere between the first and last differing byte. */

        split_at = f_diff + rand_below(afl, l_diff - f_diff);

        /* Do the thing. */

        len = target->len;
        afl->in_scratch_buf = afl_realloc(AFL_BUF_PARAM(in_scratch), len);
        memcpy(afl->in_scratch_buf, in_buf, split_at);
        memcpy(afl->in_scratch_buf + split_at, new_buf, len - split_at);
        in_buf = afl->in_scratch_buf;
        afl_swap_bufs(AFL_BUF_PARAM(in), AFL_BUF_PARAM(in_scratch));

        out_buf = afl_realloc(AFL_BUF_PARAM(out), len);
        if (unlikely(!out_buf)) { PFATAL("alloc"); }
        memcpy(out_buf, in_buf, len);

        goto havoc_stage_puppet;

      }                                                  /* if splice_cycle */

#endif                                                     /* !IGNORE_FINDS */

      ret_val = 0;

    abandon_entry:
    abandon_entry_puppet:

      if ((s64)splice_cycle >= afl->SPLICE_CYCLES_puppet) {

        afl->SPLICE_CYCLES_puppet =
            (rand_below(
                 afl, SPLICE_CYCLES_puppet_up - SPLICE_CYCLES_puppet_low + 1) +
             SPLICE_CYCLES_puppet_low);

      }

      afl->splicing_with = -1;

      /* Update afl->pending_not_fuzzed count if we made it through the
         calibration cycle and have not seen this entry before. */
      /*
        // TODO FIXME: I think we need this plus need an -L -1 check
        if (!afl->stop_soon && !afl->queue_cur->cal_failed &&
            (afl->queue_cur->was_fuzzed == 0 || afl->queue_cur->fuzz_level == 0)
        && !afl->queue_cur->disabled) {

          if (!afl->queue_cur->was_fuzzed) {

            --afl->pending_not_fuzzed;
            afl->queue_cur->was_fuzzed = 1;
            if (afl->queue_cur->favored) { --afl->pending_favored; }

          }

        }

      */

      orig_in = NULL;

      if (afl->key_puppet == 1) {

        if (unlikely(
                afl->queued_paths + afl->unique_crashes >
                ((afl->queued_paths + afl->unique_crashes) * limit_time_bound +
                 afl->orig_hit_cnt_puppet))) {

          afl->key_puppet = 0;
          afl->orig_hit_cnt_puppet = 0;
          afl->last_limit_time_start = 0;

        }

      }

      if (unlikely(*MOpt_globals.pTime > MOpt_globals.period)) {

        afl->total_pacemaker_time += *MOpt_globals.pTime;
        *MOpt_globals.pTime = 0;
        new_hit_cnt = afl->queued_paths + afl->unique_crashes;

        if (MOpt_globals.is_pilot_mode) {

          afl->swarm_fitness[afl->swarm_now] =
              (double)(afl->total_puppet_find - afl->temp_puppet_find) /
              ((double)(afl->tmp_pilot_time) / afl->period_pilot_tmp);

        }

        afl->temp_puppet_find = afl->total_puppet_find;
        u64 temp_stage_finds_puppet = 0;
        for (i = 0; i < operator_num; ++i) {

          if (MOpt_globals.is_pilot_mode) {

            double temp_eff = 0.0;

            if (MOpt_globals.cycles_v2[i] > MOpt_globals.cycles[i]) {

              temp_eff =
                  (double)(MOpt_globals.finds_v2[i] - MOpt_globals.finds[i]) /
                  (double)(MOpt_globals.cycles_v2[i] - MOpt_globals.cycles[i]);

            }

            if (afl->eff_best[afl->swarm_now][i] < temp_eff) {

              afl->eff_best[afl->swarm_now][i] = temp_eff;
              afl->L_best[afl->swarm_now][i] = afl->x_now[afl->swarm_now][i];

            }

          }

          MOpt_globals.finds[i] = MOpt_globals.finds_v2[i];
          MOpt_globals.cycles[i] = MOpt_globals.cycles_v2[i];
          temp_stage_finds_puppet += MOpt_globals.finds[i];

        }                                    /* for i = 0; i < operator_num */

        if (MOpt_globals.is_pilot_mode) {

          afl->swarm_now = afl->swarm_now + 1;
          if (afl->swarm_now == swarm_num) {

            afl->key_module = 1;
            for (i = 0; i < operator_num; ++i) {

              afl->core_operator_cycles_puppet_v2[i] =
                  afl->core_operator_cycles_puppet[i];
              afl->core_operator_cycles_puppet_v3[i] =
                  afl->core_operator_cycles_puppet[i];
              afl->core_operator_finds_puppet_v2[i] =
                  afl->core_operator_finds_puppet[i];

            }

            double swarm_eff = 0.0;
            afl->swarm_now = 0;
            for (i = 0; i < swarm_num; ++i) {

              if (afl->swarm_fitness[i] > swarm_eff) {

                swarm_eff = afl->swarm_fitness[i];
                afl->swarm_now = i;

              }

            }

            if (afl->swarm_now < 0 || afl->swarm_now > swarm_num - 1) {

              PFATAL("swarm_now error number  %d", afl->swarm_now);

            }

          }                               /* if afl->swarm_now == swarm_num */

          /* adjust pointers dependent on 'afl->swarm_now' */
          afl->mopt_globals_pilot.finds =
              afl->stage_finds_puppet[afl->swarm_now];
          afl->mopt_globals_pilot.finds_v2 =
              afl->stage_finds_puppet_v2[afl->swarm_now];
          afl->mopt_globals_pilot.cycles =
              afl->stage_cycles_puppet[afl->swarm_now];
          afl->mopt_globals_pilot.cycles_v2 =
              afl->stage_cycles_puppet_v2[afl->swarm_now];
          afl->mopt_globals_pilot.cycles_v3 =
              afl->stage_cycles_puppet_v3[afl->swarm_now];

        } else {

          for (i = 0; i < operator_num; i++) {

            afl->core_operator_finds_puppet[i] =
                afl->core_operator_finds_puppet_v2[i];
            afl->core_operator_cycles_puppet[i] =
                afl->core_operator_cycles_puppet_v2[i];
            temp_stage_finds_puppet += afl->core_operator_finds_puppet[i];

          }

          afl->key_module = 2;

          afl->old_hit_count = new_hit_cnt;

        }                                                  /* if pilot_mode */

      }         /* if (unlikely(*MOpt_globals.pTime > MOpt_globals.period)) */

    }                                                              /* block */

  }                                                                /* block */

  return ret_val;

}

#undef FLIP_BIT

u8 core_fuzzing(afl_state_t *afl) {

  return mopt_common_fuzzing(afl, afl->mopt_globals_core);

}

u8 pilot_fuzzing(afl_state_t *afl) {

  return mopt_common_fuzzing(afl, afl->mopt_globals_pilot);

}

void pso_updating(afl_state_t *afl) {

  afl->g_now++;
  if (afl->g_now > afl->g_max) { afl->g_now = 0; }
  afl->w_now =
      (afl->w_init - afl->w_end) * (afl->g_max - afl->g_now) / (afl->g_max) +
      afl->w_end;
  int tmp_swarm, i, j;
  u64 temp_operator_finds_puppet = 0;
  for (i = 0; i < operator_num; ++i) {

    afl->operator_finds_puppet[i] = afl->core_operator_finds_puppet[i];

    for (j = 0; j < swarm_num; ++j) {

      afl->operator_finds_puppet[i] =
          afl->operator_finds_puppet[i] + afl->stage_finds_puppet[j][i];

    }

    temp_operator_finds_puppet =
        temp_operator_finds_puppet + afl->operator_finds_puppet[i];

  }

  for (i = 0; i < operator_num; ++i) {

    if (afl->operator_finds_puppet[i]) {

      afl->G_best[i] = (double)((double)(afl->operator_finds_puppet[i]) /
                                (double)(temp_operator_finds_puppet));

    }

  }

  for (tmp_swarm = 0; tmp_swarm < swarm_num; ++tmp_swarm) {

    double x_temp = 0.0;
    for (i = 0; i < operator_num; ++i) {

      afl->probability_now[tmp_swarm][i] = 0.0;
      afl->v_now[tmp_swarm][i] =
          afl->w_now * afl->v_now[tmp_swarm][i] +
          RAND_C * (afl->L_best[tmp_swarm][i] - afl->x_now[tmp_swarm][i]) +
          RAND_C * (afl->G_best[i] - afl->x_now[tmp_swarm][i]);
      afl->x_now[tmp_swarm][i] += afl->v_now[tmp_swarm][i];
      if (afl->x_now[tmp_swarm][i] > v_max) {

        afl->x_now[tmp_swarm][i] = v_max;

      } else if (afl->x_now[tmp_swarm][i] < v_min) {

        afl->x_now[tmp_swarm][i] = v_min;

      }

      x_temp += afl->x_now[tmp_swarm][i];

    }

    for (i = 0; i < operator_num; ++i) {

      afl->x_now[tmp_swarm][i] = afl->x_now[tmp_swarm][i] / x_temp;
      if (likely(i != 0)) {

        afl->probability_now[tmp_swarm][i] =
            afl->probability_now[tmp_swarm][i - 1] + afl->x_now[tmp_swarm][i];

      } else {

        afl->probability_now[tmp_swarm][i] = afl->x_now[tmp_swarm][i];

      }

    }

    if (afl->probability_now[tmp_swarm][operator_num - 1] < 0.99 ||
        afl->probability_now[tmp_swarm][operator_num - 1] > 1.01) {

      FATAL("ERROR probability");

    }

  }

  afl->swarm_now = 0;
  afl->key_module = 0;

}

/* larger change for MOpt implementation: the original fuzz_one was renamed
   to fuzz_one_original. All documentation references to fuzz_one therefore
   mean fuzz_one_original */

u8 fuzz_one(afl_state_t *afl) {

  return fuzz_one_original(afl);

// Dont use MOpt stuff
  int key_val_lv_1 = 0, key_val_lv_2 = 0;

#ifdef _AFL_DOCUMENT_MUTATIONS

  u8 path_buf[PATH_MAX];
  if (afl->do_document == 0) {

    snprintf(path_buf, PATH_MAX, "%s/mutations", afl->out_dir);
    afl->do_document = mkdir(path_buf, 0700);  // if it exists we do not care
    afl->do_document = 1;

  } else {

    afl->do_document = 2;
    afl->stop_soon = 2;

  }

#endif

  // if limit_time_sig == -1 then both are run after each other

  if (afl->limit_time_sig <= 0) { key_val_lv_1 = fuzz_one_original(afl); }

  if (afl->limit_time_sig != 0) {

    if (afl->key_module == 0) {

      key_val_lv_2 = pilot_fuzzing(afl);

    } else if (afl->key_module == 1) {

      key_val_lv_2 = core_fuzzing(afl);

    } else if (afl->key_module == 2) {

      pso_updating(afl);

    }

  }

  return (key_val_lv_1 | key_val_lv_2);

}

