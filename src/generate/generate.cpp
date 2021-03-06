/*
 * Copyright 2018-2019 NWO-I
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include <iostream>
#include <array>
#include <numeric>
#include <random>
#include <stdexcept>
#include <vector>
#include <tuple>
#include <sstream>

#include <generate.h>
#include <storage.h>


namespace {
  using std::cout;
  using std::pair;
  using std::vector;
  using std::array;
}

unsigned int ref_dom(int dom, int mod) {
  return 100 * (dom + 1) + mod + 1;
}

unsigned int orca_dom(int dom, int mod) {
  return 18 * dom + mod + 1;
}

Generators::Generators(const int seed0, const int seed1, const std::array<float, 4> rates)
  : m_seeds{seed0, seed1},
    m_rates{std::move(rates)},
    m_seed_seq{{seed0, seed1}},
    mt{m_seed_seq},
    coincidence_rate{std::accumulate(std::next(begin(m_rates)),
                                     end(m_rates), 0.0)},
    // p = 1 - exp(-lambda)
    coincidence{1. - exp(-coincidence_rate / 1e9)},
    flat{0., 1.}
{
#ifdef USE_AVX2
   using namespace GenAVX2;
#else
   using namespace GenScalar;
#endif

   if (m_rates[0] < 1.f) {
      throw std::domain_error{"L0 rate must be > 1 Hz"};
   }

   // probabilities for coincidences
   for (size_t i = 0; i < prob1D.size(); ++i) {
      prob2D[i][i] = 0.0;
      for (size_t j = i + 1; j < prob1D.size(); ++j) {
         const float ct = dot_product(PMTs[i], PMTs[j]);
         const float p  = cross_prob(ct);
         prob2D[i][j] = p;
         prob2D[j][i] = p;
      }
   }

   // determine probability of a coincidence as a function of the PMT number
   for (size_t i = 0; i < prob1D.size(); ++i) {
      prob1D[i] = std::accumulate(begin(prob2D[i]), end(prob2D[i]), 0.);
   }
}

float cross_prob(const float ct) {
   return std::exp(ct * (Constants::p2 + ct * (Constants::p3 + ct * Constants::p4)));
}

std::tuple<size_t, size_t>
fill_coincidences(storage_t& times, pmts_t& pmts, size_t idx,
                  const long time_start, const long time_end,
                  Generators& gen) {
  const auto& prob1D = gen.prob1D;
  const auto& prob2D = gen.prob2D;
  auto& probND = gen.probND;
  auto& mt = gen.mt;
  auto& flat = gen.flat;

  std::fill(begin(pmts), end(pmts), 0);

  if (gen.coincidence_rate < 0.001) {
    return {0, 0};
  }
  pmts.clear();

  // Fill coincidences
  size_t n = 0;
  for (long t1 = time_start ; t1 < time_end; t1 += gen.coincidence(mt)) {
    ++n;
    // generate two-fold coincidence
    const unsigned int pmt1 = random_index(prob1D, flat(mt));
    const unsigned int pmt2 = random_index(prob2D[pmt1], flat(mt));

    pmts.emplace_back(pmt1);
    pmts.emplace_back(pmt2);

    std::normal_distribution<double> gauss(t1, 0.5);
    times[++idx] = std::lround(gauss(mt));
    times[++idx] = std::lround(gauss(mt));
    try {
      // generate larger than two-fold coincidences, if any
      unsigned int M = random_index(gen.rates(), flat(mt));
      if (M != 0) {
        probND = prob2D[pmt1];

        for (unsigned int pmtN = pmt2; M != 0; --M) {
          for (size_t i = 0; i != prob2D[pmtN].size(); ++i) {
            probND[i] *= prob2D[pmtN][i];
          }

          probND[pmtN] = 0.0;
          pmtN = random_index(probND, flat(mt));
          pmts.emplace_back(pmtN);
          times[++idx] = std::lround(gauss(mt));
        }
      }
    }
    catch (const std::domain_error&) {}
  }
  return {pmts.size(), n};
}

std::tuple<storage_t, storage_t> generate(const long start, const long stop,
                                          Generators& gens, std::string dom_fun,
                                          bool use_avx2) {
  auto it = Constants::dom_funs.find(dom_fun);
  std::stringstream funs;
  std::for_each(begin(Constants::dom_funs), end(Constants::dom_funs),
                [&funs] (const auto& entry) {
                  funs << " " << entry.first;
                });
  if (it == end(Constants::dom_funs)) {
    throw std::runtime_error{std::string{"wrong dom mapping function, should be one of: "} +
                             funs.str()};
  }

#ifdef USE_AVX2
   if (use_avx2) {
     return generate_avx2(start, stop, gens, it->second);
   } else {
     return generate_scalar(start, stop, gens, it->second);
   }
#else
   return generate_scalar(start, stop, gens, it->second);
#endif
}
