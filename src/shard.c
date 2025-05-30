/*
 * ZMap Copyright 2013 Regents of the University of Michigan
 *
 * Licensed under the Apache License, Version 2.0 (the "License"); you may not
 * use this file except in compliance with the License. You may obtain a copy
 * of the License at http://www.apache.org/licenses/LICENSE-2.0
 */

#include <stdint.h>
#include <assert.h>

#include <gmp.h>

#include "../lib/includes.h"
#include "../lib/logger.h"
#include "../lib/blocklist.h"
#include "shard.h"
#include "state.h"

static inline uint16_t extract_port(uint64_t v, uint8_t bits)
{
	uint64_t mask = (1 << bits) - 1;
	return (uint16_t)(v & mask);
}

static inline uint32_t extract_ip(uint64_t v, uint8_t bits)
{
	return (uint32_t)(v >> bits);
}

static void shard_roll_to_valid(shard_t *s)
{
	uint64_t current_ip_index = (s->current - 1) >> s->bits_for_port;
	uint16_t candidate_port = extract_port(s->current - 1, s->bits_for_port);
	if (current_ip_index < zsend.max_ip_index && candidate_port < zconf.ports->port_count) {
		return;
	}
	shard_get_next_target(s);
}

void shard_init(shard_t *shard, uint16_t shard_idx, uint16_t num_shards,
		uint8_t thread_idx, uint8_t num_threads,
		uint64_t max_total_targets, uint8_t bits_for_port,
		const cycle_t *cycle, shard_complete_cb cb, void *arg)
{
	// Start out by figuring out how many shards we have. A single shard of
	// ZMap (set with --shards=N, --shard=n) may have several subshards, if
	// ZMap is being ran multithreaded (set with --sender-threads=T).
	//
	// Total number of subshards is S = N*T. Subshard ID's range from [0,
	// N*T).
	assert(num_shards > 0);
	assert(num_threads > 0);
	assert(shard_idx < num_shards);
	assert(thread_idx < num_threads);
	uint32_t num_subshards = (uint32_t)num_shards * (uint32_t)num_threads;
	uint64_t num_elts = cycle->order;
	assert(num_subshards < num_elts);
	assert(!max_total_targets || (num_subshards <= max_total_targets));

	// This instance of ZMap will run T subshards, with one subshard per
	// thread. This composes a single shard, as specified by the command
	// line flag --shard=n.  E.g. to run shard with index n, we must run
	// subshards with indices the range [n*T, (n+1)*T].
	//
	// We can calculate our subshard index i = n*T + t.
	uint32_t sub_idx = shard_idx * num_threads + thread_idx;

	// Given i, we want to calculate the start of subshard i. Subshards
	// define ranges over exponents of g. They range from [0, Q-1), where Q
	// is the number of elements in (the order of) the group generated by
	// g.
	//
	// Let e_b = floor(Q / S) * i
	uint64_t exponent_begin = (num_elts / num_subshards) * sub_idx;

	// The stopping exponent is the first element of the next shard.
	//
	//     e_e = floor(Q / S) * ((i + 1) % S)
	uint64_t exponent_end =
	    (num_elts / num_subshards) * ((sub_idx + 1) % num_subshards);

	// We actually offset the begin and end of each cycle. Given an offset
	// k, shift each exponent by k modulo Q.
	exponent_begin = (exponent_begin + cycle->offset) % num_elts;
	exponent_end = (exponent_end + cycle->offset) % num_elts;

	// Multiprecision variants of everything above
	mpz_t generator_m, exponent_begin_m, exponent_end_m, prime_m;
	mpz_init_set_ui(generator_m, cycle->generator);
	mpz_init_set_ui(exponent_begin_m, exponent_begin);
	mpz_init_set_ui(exponent_end_m, exponent_end);
	mpz_init_set_ui(prime_m, cycle->group->prime);

	// Calculate the first and last points of the shard as powers of g
	// modulo p.
	mpz_t start_m, stop_m;
	mpz_init(start_m);
	mpz_init(stop_m);
	mpz_powm(start_m, generator_m, exponent_begin_m, prime_m);
	mpz_powm(stop_m, generator_m, exponent_end_m, prime_m);

	// Pull the result out as a uint64_t
	shard->params.first = (uint64_t)mpz_get_ui(start_m);
	shard->params.last = (uint64_t)mpz_get_ui(stop_m);
	shard->params.factor = cycle->generator;
	shard->params.modulus = cycle->group->prime;
	shard->bits_for_port = bits_for_port;

	// Set the shard at the beginning.
	shard->current = shard->params.first;

	// Set the (thread) id
	shard->thread_id = thread_idx;

	// Set max_targets if applicable
	if (max_total_targets > 0) {
		uint64_t max_targets_this_shard =
		    max_total_targets / num_subshards;
		if (sub_idx < (max_total_targets % num_subshards)) {
			++max_targets_this_shard;
		}
		shard->state.max_targets = max_targets_this_shard;
	}

	// Set the callbacks
	shard->cb = cb;
	shard->arg = arg;

	// If the beginning of a shard isn't pointing to a valid index in the
	// blocklist, find the first element that is.
	shard_roll_to_valid(shard);

	// Clear everything
	mpz_clear(generator_m);
	mpz_clear(exponent_begin_m);
	mpz_clear(exponent_end_m);
	mpz_clear(prime_m);
	mpz_clear(start_m);
	mpz_clear(stop_m);
}

target_t shard_get_cur_target(shard_t *shard)
{
	if (shard->current == ZMAP_SHARD_DONE) {
		// shard_roll_to_valid() has rolled to the very end.
		return (target_t){
		    .ip = 0, .port = 0, .status = ZMAP_SHARD_DONE};
	}
	uint32_t ip = extract_ip(shard->current - 1, shard->bits_for_port);
	uint16_t port = extract_port(shard->current - 1, shard->bits_for_port);
	return (target_t){.ip = (uint32_t)blocklist_lookup_index(ip),
			  .port = (uint16_t)zconf.ports->ports[port],
			  .status = ZMAP_SHARD_OK};
}

static inline uint64_t shard_get_next_elem(shard_t *shard)
{
	shard->current *= shard->params.factor;
	shard->current %= shard->params.modulus;
	return shard->current;
}

target_t shard_get_next_target(shard_t *shard)
{
	if (shard->current == ZMAP_SHARD_DONE) {
		return (target_t){
		    .ip = 0, .port = 0, .status = ZMAP_SHARD_DONE};
	}
	while (1) {
		uint64_t candidate = shard_get_next_elem(shard);
		if (candidate == shard->params.last) {
			shard->current = ZMAP_SHARD_DONE;
			shard->iterations++;
			return (target_t){
			    .ip = 0, .port = 0, .status = ZMAP_SHARD_DONE};
		}
		if (candidate >= zsend.max_target_index) {
			// If the candidate is out of bounds, re-roll. This will happen since we choose primes/moduli that are
			// larger than the number of allowed targets. The IP is bounded below by checking against zsend.max_ip_index.
			continue;
		}
		// Good candidate, proceed with it.
		uint32_t candidate_ip =
		    extract_ip(candidate - 1, shard->bits_for_port);
		uint16_t candidate_port =
		    extract_port(candidate - 1, shard->bits_for_port);
		if (candidate_ip < zsend.max_ip_index &&
		    candidate_port < zconf.ports->port_count) {
			shard->iterations++;
			return (target_t){
			    .ip = blocklist_lookup_index(candidate_ip),
			    .port = zconf.ports->ports[candidate_port],
			    .status = ZMAP_SHARD_OK};
		}
	}
}
