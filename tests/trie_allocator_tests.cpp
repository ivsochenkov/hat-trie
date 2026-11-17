/**
 * MIT License
 *
 * Copyright (c) 2017 Thibaut Goetghebuer-Planchon <tessil@gmx.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

/**
 * Tests of the stateful allocators and fancy pointers support:
 *  - a stateful allocator (raw pointers) that checks everything it is given
 *    back (exact sizes), counts, can fail on demand and has configurable
 *    propagation traits,
 *  - Boost.Interprocess allocators (offset_ptr): the structures are built in a
 *    segment, the segment is copied/mapped at another address and used there.
 */
// std::getenv is flagged as unsafe by MSVC (an error with /WX).
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif

#include <algorithm>
#include <boost/interprocess/allocators/allocator.hpp>
#include <boost/interprocess/containers/string.hpp>
#include <boost/interprocess/managed_external_buffer.hpp>
#include <boost/interprocess/managed_mapped_file.hpp>
#include <boost/interprocess/managed_shared_memory.hpp>
#include <boost/mpl/list.hpp>
#include <boost/test/unit_test.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <map>
#include <memory>
#include <new>
#include <random>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "tsl/array-hash/array_map.h"
#include "tsl/array-hash/array_set.h"
#include "tsl/htrie_map.h"
#include "tsl/htrie_set.h"
#include "utils.h"

namespace {

namespace pol = tsl::htrie_policy;
namespace bip = boost::interprocess;

/* ===========================================================================
 * Stateful allocator with raw pointers
 * ===========================================================================
 */

/**
 * Memory resource behind the allocators. Tracks every block (and its size).
 */
struct arena {
  std::map<void*, std::size_t> blocks;
  std::size_t live_bytes = 0;
  std::size_t total_allocations = 0;
  std::size_t bad_deallocations = 0;

  /**
   * Throw std::bad_alloc when this number of allocations has been done (the
   * allocation number fail_after, starting at 0, fails). Negative: never.
   */
  long fail_after = -1;

  /**
   * Arena used by select_on_container_copy_construction (null: same arena).
   */
  arena* copy_arena = nullptr;

  arena() = default;
  arena(const arena&) = delete;
  arena& operator=(const arena&) = delete;

  ~arena() {
    for (auto& block : blocks) {
      ::operator delete(block.first);
    }
  }
};

template <class T, bool Propagate>
class arena_allocator {
  template <class U, bool P>
  friend class arena_allocator;

 public:
  using value_type = T;
  using propagate_on_container_copy_assignment =
      std::integral_constant<bool, Propagate>;
  using propagate_on_container_move_assignment =
      std::integral_constant<bool, Propagate>;
  using propagate_on_container_swap = std::integral_constant<bool, Propagate>;

  template <class U>
  struct rebind {
    using other = arena_allocator<U, Propagate>;
  };

  explicit arena_allocator(arena& a) noexcept : m_arena(&a) {}

  template <class U>
  arena_allocator(const arena_allocator<U, Propagate>& other) noexcept
      : m_arena(other.m_arena) {}

  T* allocate(std::size_t n) {
    if (m_arena->fail_after >= 0 && m_arena->fail_after-- == 0) {
      throw std::bad_alloc();
    }

    void* ptr = ::operator new(n * sizeof(T));
    // Garbage: nothing may rely on the memory being zeroed.
    std::memset(ptr, 0xCD, n * sizeof(T));

    m_arena->blocks[ptr] = n * sizeof(T);
    m_arena->live_bytes += n * sizeof(T);
    m_arena->total_allocations++;

    return static_cast<T*>(ptr);
  }

  void deallocate(T* ptr, std::size_t n) noexcept {
    auto it = m_arena->blocks.find(ptr);
    if (it == m_arena->blocks.end()) {
      m_arena->bad_deallocations++;  // not ours (or freed twice)
      return;
    }

    if (it->second != n * sizeof(T)) {
      m_arena->bad_deallocations++;  // wrong size
    }

    m_arena->live_bytes -= it->second;
    m_arena->blocks.erase(it);
    ::operator delete(ptr);
  }

  arena_allocator select_on_container_copy_construction() const {
    return (m_arena->copy_arena != nullptr)
               ? arena_allocator(*m_arena->copy_arena)
               : *this;
  }

  template <class U>
  bool operator==(const arena_allocator<U, Propagate>& other) const noexcept {
    return m_arena == other.m_arena;
  }

  template <class U>
  bool operator!=(const arena_allocator<U, Propagate>& other) const noexcept {
    return m_arena != other.m_arena;
  }

 private:
  arena* m_arena;
};

using hash_char = tsl::detail_htrie_hash::default_hash<char>;
using hash_char32 = tsl::detail_htrie_hash::default_hash<char32_t>;

template <class CharT, class T, class Policy, bool Propagate = false>
using arena_map =
    tsl::htrie_map<CharT, T, tsl::detail_htrie_hash::default_hash<CharT>,
                   std::uint16_t, Policy, arena_allocator<char, Propagate>>;

template <class CharT, class Policy, bool Propagate = false>
using arena_set =
    tsl::htrie_set<CharT, tsl::detail_htrie_hash::default_hash<CharT>,
                   std::uint16_t, Policy, arena_allocator<char, Propagate>>;

/**
 * Policies that can be used with any allocator for 8 bits letters (std_map is
 * for raw pointers allocators, which is what the arena allocator uses).
 */
using policies_8bits =
    boost::mpl::list<pol::dense_array_children, pol::flat_map_children,
                     pol::bitmap_children, pol::std_map_children>;

/**
 * Policies for wider letters, usable with raw pointers allocators.
 */
using policies_wide =
    boost::mpl::list<pol::flat_map_children, pol::std_map_children>;

/**
 * Policies that can also be used with fancy pointers.
 */
using policies_fancy_8bits =
    boost::mpl::list<pol::dense_array_children, pol::flat_map_children,
                     pol::bitmap_children>;

/* ===========================================================================
 * Helpers
 * ===========================================================================
 */

/**
 * Deterministic random keys (shared prefixes, different lengths, the empty
 * key), whatever the letter type.
 */
template <class Key>
std::vector<Key> make_keys(std::size_t nb_keys, unsigned seed = 42) {
  using letter = typename Key::value_type;
  const unsigned first_letter = (sizeof(letter) == 1) ? unsigned('a') : 0x4E00u;

  std::mt19937 rng(seed);
  std::vector<Key> keys;
  keys.push_back(Key());  // the empty key

  while (keys.size() < nb_keys) {
    Key key;
    // Few distinct prefixes: deep tries with long common prefixes.
    key.push_back(static_cast<letter>(first_letter + rng() % 3));
    key.push_back(static_cast<letter>(first_letter + rng() % 5));

    const std::size_t length = rng() % 14;
    for (std::size_t i = 0; i < length; i++) {
      key.push_back(static_cast<letter>(first_letter + rng() % 26));
    }

    keys.push_back(std::move(key));
  }

  return keys;
}

template <class Map>
using key_of = typename Map::key_buffer_type;

template <class Map>
using model_of = std::map<key_of<Map>, std::int64_t>;

template <class Map>
void fill(Map& map, model_of<Map>& model, std::size_t nb_keys,
          unsigned seed = 42) {
  std::int64_t counter = 0;
  for (const key_of<Map>& key : make_keys<key_of<Map>>(nb_keys, seed)) {
    const bool inserted = map.insert(key, counter).second;
    BOOST_CHECK_EQUAL(inserted, model.emplace(key, counter).second);
    counter++;
  }
}

template <class Map>
void check_matches(const Map& map, const model_of<Map>& model) {
  BOOST_CHECK_EQUAL(map.size(), model.size());

  for (const auto& kv : model) {
    auto it = map.find(kv.first);
    BOOST_REQUIRE(it != map.end());
    BOOST_CHECK_EQUAL(it.value(), kv.second);
  }

  // Iteration: every element exactly once (the order inside a hash node is
  // unspecified).
  model_of<Map> iterated;
  for (auto it = map.begin(); it != map.end(); ++it) {
    BOOST_CHECK(iterated.emplace(it.key(), it.value()).second);
  }
  BOOST_CHECK(iterated == model);
}

template <class Set>
void check_set_matches(const Set& set,
                       const std::map<key_of<Set>, int>& model) {
  BOOST_CHECK_EQUAL(set.size(), model.size());

  for (const auto& kv : model) {
    BOOST_CHECK(set.find(kv.first) != set.end());
  }

  std::size_t nb_iterated = 0;
  for (auto it = set.begin(); it != set.end(); ++it) {
    BOOST_CHECK(model.find(it.key()) != model.end());
    nb_iterated++;
  }
  BOOST_CHECK_EQUAL(nb_iterated, model.size());
}

template <class Map>
void erase_every_other(Map& map, model_of<Map>& model) {
  bool erase_it = true;
  for (auto it = model.begin(); it != model.end();) {
    if (erase_it) {
      BOOST_CHECK_EQUAL(map.erase(it->first), 1u);
      it = model.erase(it);
    } else {
      ++it;
    }
    erase_it = !erase_it;
  }
}

void check_arena_is_clean(const arena& ar) {
  BOOST_CHECK_EQUAL(ar.live_bytes, 0u);
  BOOST_CHECK_EQUAL(ar.blocks.size(), 0u);
  BOOST_CHECK_EQUAL(ar.bad_deallocations, 0u);
}

/* ===========================================================================
 * Stateful allocator: behaviour
 * ===========================================================================
 */

BOOST_AUTO_TEST_SUITE(test_htrie_stateful_allocator)

BOOST_AUTO_TEST_CASE_TEMPLATE(test_all_the_memory_comes_from_the_allocator, P,
                              policies_8bits) {
  using map_type = arena_map<char, std::int64_t, P>;
  using alloc_type = arena_allocator<char, false>;

  arena ar;
  {
    map_type map(std::size_t(8), hash_char(), alloc_type(ar));
    BOOST_CHECK(map.get_allocator() == alloc_type(ar));
    BOOST_CHECK_EQUAL(ar.live_bytes, 0u);

    model_of<map_type> model;
    fill(map, model, 4000);
    check_matches(map, model);
    BOOST_CHECK(ar.live_bytes > 0);

    erase_every_other(map, model);
    check_matches(map, model);

    map.shrink_to_fit();
    check_matches(map, model);

    BOOST_CHECK_EQUAL(map.erase_prefix("a"), [&] {
      std::size_t nb = 0;
      for (auto it = model.begin(); it != model.end();) {
        if (!it->first.empty() && it->first[0] == 'a') {
          it = model.erase(it);
          nb++;
        } else {
          ++it;
        }
      }
      return nb;
    }());
    check_matches(map, model);

    map.clear();
    BOOST_CHECK(map.empty());
    check_arena_is_clean(ar);

    // Still usable after a clear.
    model.clear();
    fill(map, model, 500, 7);
    check_matches(map, model);
  }

  check_arena_is_clean(ar);
}

BOOST_AUTO_TEST_CASE_TEMPLATE(test_set, P, policies_8bits) {
  using set_type = arena_set<char, P>;
  using alloc_type = arena_allocator<char, false>;

  arena ar;
  {
    set_type set(std::size_t(8), hash_char(), alloc_type(ar));
    std::map<std::string, int> model;
    for (const std::string& key : make_keys<std::string>(3000)) {
      BOOST_CHECK_EQUAL(set.insert(key).second, model.emplace(key, 0).second);
    }
    check_set_matches(set, model);
    BOOST_CHECK(ar.live_bytes > 0);

    set_type copy(set);
    check_set_matches(copy, model);
    set.clear();
    check_set_matches(copy, model);
  }

  check_arena_is_clean(ar);
}

BOOST_AUTO_TEST_CASE_TEMPLATE(test_wide_letters, P, policies_wide) {
  using map_type = arena_map<char32_t, std::int64_t, P>;
  using alloc_type = arena_allocator<char, false>;

  arena ar;
  {
    map_type map(std::size_t(8), hash_char32(), alloc_type(ar));

    model_of<map_type> model;
    fill(map, model, 3000);
    check_matches(map, model);

    erase_every_other(map, model);
    check_matches(map, model);

    map_type copy(map);
    check_matches(copy, model);
  }

  check_arena_is_clean(ar);
}

BOOST_AUTO_TEST_CASE_TEMPLATE(test_copy_constructors, P, policies_8bits) {
  using map_type = arena_map<char, std::int64_t, P>;
  using alloc_type = arena_allocator<char, false>;

  arena ar, ar_copy, ar_other;
  {
    map_type map(std::size_t(8), hash_char(), alloc_type(ar));
    model_of<map_type> model;
    fill(map, model, 2000);

    // select_on_container_copy_construction gives another arena.
    ar.copy_arena = &ar_copy;
    const std::size_t nb_allocations_before = ar.total_allocations;
    map_type copy(map);
    BOOST_CHECK(copy.get_allocator() == alloc_type(ar_copy));
    BOOST_CHECK_EQUAL(ar.total_allocations, nb_allocations_before);
    BOOST_CHECK(ar_copy.live_bytes > 0);
    check_matches(copy, model);

    // Explicit allocator.
    map_type copy2(map, alloc_type(ar_other));
    BOOST_CHECK(copy2.get_allocator() == alloc_type(ar_other));
    BOOST_CHECK(ar_other.live_bytes > 0);
    check_matches(copy2, model);

    // The copies are independent.
    map.clear();
    check_matches(copy, model);
    check_matches(copy2, model);
    check_arena_is_clean(ar);
  }

  check_arena_is_clean(ar_copy);
  check_arena_is_clean(ar_other);
}

BOOST_AUTO_TEST_CASE_TEMPLATE(test_move_constructors, P, policies_8bits) {
  using map_type = arena_map<char, std::int64_t, P>;
  using alloc_type = arena_allocator<char, false>;

  arena ar, ar_other;
  {
    map_type map(std::size_t(8), hash_char(), alloc_type(ar));
    model_of<map_type> model;
    fill(map, model, 2000);

    // Move: the memory is taken, nothing allocated.
    const std::size_t nb_allocations_before = ar.total_allocations;
    map_type moved(std::move(map));
    BOOST_CHECK_EQUAL(ar.total_allocations, nb_allocations_before);
    BOOST_CHECK(moved.get_allocator() == alloc_type(ar));
    BOOST_CHECK(map.empty());
    check_matches(moved, model);

    // Extended move with an equal allocator: still no allocation.
    map_type moved2(std::move(moved), alloc_type(ar));
    BOOST_CHECK_EQUAL(ar.total_allocations, nb_allocations_before);
    BOOST_CHECK(moved.empty());
    check_matches(moved2, model);

    // Extended move with another arena: the elements are moved one by one in
    // the other arena.
    map_type moved3(std::move(moved2), alloc_type(ar_other));
    BOOST_CHECK(moved3.get_allocator() == alloc_type(ar_other));
    BOOST_CHECK(moved2.empty());
    BOOST_CHECK(ar_other.live_bytes > 0);
    check_matches(moved3, model);

    // The source of the move can be reused.
    model_of<map_type> model2;
    fill(moved2, model2, 100, 5);
    check_matches(moved2, model2);
  }

  check_arena_is_clean(ar);
  check_arena_is_clean(ar_other);
}

BOOST_AUTO_TEST_CASE(test_move_only_values_in_another_arena) {
  using map_type = arena_map<char, move_only_test, pol::flat_map_children>;
  using alloc_type = arena_allocator<char, false>;

  arena ar, ar_other;
  {
    map_type map(std::size_t(8), hash_char(), alloc_type(ar));
    for (std::size_t i = 0; i < 1000; i++) {
      map.insert(utils::get_key<char>(i), utils::get_value<move_only_test>(i));
    }

    map_type moved(std::move(map), alloc_type(ar_other));
    BOOST_CHECK_EQUAL(moved.size(), 1000u);
    BOOST_CHECK(map.empty());
    for (std::size_t i = 0; i < 1000; i++) {
      auto it = moved.find(utils::get_key<char>(i));
      BOOST_REQUIRE(it != moved.end());
      BOOST_CHECK(it.value() == utils::get_value<move_only_test>(i));
    }

    // Move assignment from another arena (the allocators don't propagate).
    map_type other(std::size_t(8), hash_char(), alloc_type(ar));
    other = std::move(moved);
    BOOST_CHECK(other.get_allocator() == alloc_type(ar));
    BOOST_CHECK_EQUAL(other.size(), 1000u);
    BOOST_CHECK(moved.empty());
    for (std::size_t i = 0; i < 1000; i++) {
      auto it = other.find(utils::get_key<char>(i));
      BOOST_REQUIRE(it != other.end());
      BOOST_CHECK(it.value() == utils::get_value<move_only_test>(i));
    }
  }

  check_arena_is_clean(ar);
  check_arena_is_clean(ar_other);
}

template <bool Propagate, class P>
void test_assignments_and_swap() {
  using map_type = arena_map<char, std::int64_t, P, Propagate>;
  using alloc_type = arena_allocator<char, Propagate>;

  arena ar_a, ar_b;
  {
    map_type a(std::size_t(8), hash_char(), alloc_type(ar_a));
    map_type b(std::size_t(16), hash_char(), alloc_type(ar_b));
    model_of<map_type> model_a, model_b;
    fill(a, model_a, 1500, 1);
    fill(b, model_b, 1000, 2);

    // Copy assignment.
    a = b;
    check_matches(a, model_b);
    check_matches(b, model_b);
    BOOST_CHECK_EQUAL(a.burst_threshold(), b.burst_threshold());
    BOOST_CHECK(a.get_allocator() == alloc_type(Propagate ? ar_b : ar_a));
    BOOST_CHECK(b.get_allocator() == alloc_type(ar_b));

    // Move assignment.
    map_type c(std::size_t(8), hash_char(), alloc_type(ar_a));
    model_of<map_type> model_c;
    fill(c, model_c, 800, 3);
    c = std::move(b);
    check_matches(c, model_b);
    BOOST_CHECK(b.empty());
    BOOST_CHECK(c.get_allocator() == alloc_type(Propagate ? ar_b : ar_a));

    // The moved from map is usable.
    model_of<map_type> model_b2;
    fill(b, model_b2, 300, 4);
    check_matches(b, model_b2);

    // Self assignments.
    const map_type& c_ref = c;
    c = c_ref;
    check_matches(c, model_b);
    map_type& c_alias = c;
    c = std::move(c_alias);
    check_matches(c, model_b);
  }

  check_arena_is_clean(ar_a);
  check_arena_is_clean(ar_b);

  // Swap. Without propagation the allocators have to be equal.
  arena ar_c, ar_d;
  {
    arena& second = Propagate ? ar_d : ar_c;

    map_type a(std::size_t(8), hash_char(), alloc_type(ar_c));
    map_type b(std::size_t(16), hash_char(), alloc_type(second));
    model_of<map_type> model_a, model_b;
    fill(a, model_a, 1200, 5);
    fill(b, model_b, 700, 6);

    a.swap(b);
    check_matches(a, model_b);
    check_matches(b, model_a);
    BOOST_CHECK_EQUAL(a.burst_threshold(), 16u);
    BOOST_CHECK(a.get_allocator() == alloc_type(second));
    BOOST_CHECK(b.get_allocator() == alloc_type(ar_c));

    using std::swap;
    swap(a, b);
    check_matches(a, model_a);
    check_matches(b, model_b);
  }

  check_arena_is_clean(ar_c);
  check_arena_is_clean(ar_d);
}

BOOST_AUTO_TEST_CASE_TEMPLATE(test_allocator_propagation, P, policies_8bits) {
  test_assignments_and_swap<false, P>();
  test_assignments_and_swap<true, P>();
}

BOOST_AUTO_TEST_CASE_TEMPLATE(test_serialization_with_allocator, P,
                              policies_8bits) {
  using map_type = arena_map<char, std::int64_t, P>;
  using alloc_type = arena_allocator<char, false>;

  arena ar, ar_other;
  {
    map_type map(std::size_t(8), hash_char(), alloc_type(ar));
    model_of<map_type> model;
    fill(map, model, 2500);

    serializer serial;
    map.serialize(serial);

    for (bool hash_compatible : {true, false}) {
      deserializer dserial(serial.str());
      map_type deserialized =
          map_type::deserialize(dserial, hash_compatible, alloc_type(ar_other));
      BOOST_CHECK(deserialized.get_allocator() == alloc_type(ar_other));
      BOOST_CHECK(ar_other.live_bytes > 0);
      check_matches(deserialized, model);
    }

    check_matches(map, model);
  }

  check_arena_is_clean(ar);
  check_arena_is_clean(ar_other);
}

/**
 * Fail the n-th allocation for every n: whatever the moment of the failure
 * there must be no leak, no double free, and the map must be usable and hold
 * what has been successfully inserted.
 */
BOOST_AUTO_TEST_CASE_TEMPLATE(test_allocation_failures_on_insertion, P,
                              policies_fancy_8bits) {
  using map_type = arena_map<char, std::int64_t, P>;
  using alloc_type = arena_allocator<char, false>;

  const std::vector<std::string> keys = make_keys<std::string>(220);

  std::size_t nb_failures = 0;
  for (long fail_at = 0; fail_at < 4000; fail_at += (fail_at < 400 ? 1 : 37)) {
    arena ar;
    {
      map_type map(std::size_t(4), hash_char(), alloc_type(ar));
      model_of<map_type> model;

      ar.fail_after = fail_at;
      bool failed = false;
      for (std::size_t i = 0; i < keys.size(); i++) {
        try {
          map.insert(keys[i], std::int64_t(i));
          model.emplace(keys[i], std::int64_t(i));
        } catch (const std::bad_alloc&) {
          failed = true;
          ar.fail_after = -1;
          // The failed insertion changed nothing...
          check_matches(map, model);
          // ...and the map keeps working.
          map.insert(keys[i], std::int64_t(i));
          model.emplace(keys[i], std::int64_t(i));
        }
      }

      nb_failures += failed ? 1 : 0;
      check_matches(map, model);
    }

    check_arena_is_clean(ar);
  }

  BOOST_CHECK(nb_failures > 100);
}

BOOST_AUTO_TEST_CASE(test_allocation_failures_on_copy_and_erase) {
  using map_type = arena_map<char, std::int64_t, pol::flat_map_children>;
  using alloc_type = arena_allocator<char, false>;

  arena ar, ar_copy;
  {
    map_type map(std::size_t(4), hash_char(), alloc_type(ar));
    model_of<map_type> model;
    fill(map, model, 300);

    // Copy.
    std::size_t nb_copy_failures = 0;
    for (long fail_at = 0; fail_at < 3000; fail_at += 7) {
      ar_copy.fail_after = fail_at;
      try {
        map_type copy(map, alloc_type(ar_copy));
        check_matches(copy, model);
      } catch (const std::bad_alloc&) {
        nb_copy_failures++;
      }

      check_arena_is_clean(ar_copy);
    }
    BOOST_CHECK(nb_copy_failures > 10);

    // Copy assignment: a failure leaves the target untouched.
    map_type target(std::size_t(32), hash_char(), alloc_type(ar));
    model_of<map_type> target_model;
    fill(target, target_model, 50, 9);
    for (long fail_at = 0; fail_at < 600; fail_at += 11) {
      ar.fail_after = fail_at;
      try {
        target = map;
        ar.fail_after = -1;
        check_matches(target, model);
        target.clear();
        target_model.clear();
        fill(target, target_model, 50, 9);
      } catch (const std::bad_alloc&) {
        ar.fail_after = -1;
        check_matches(target, target_model);
      }
    }
    ar.fail_after = -1;

    // Erase may have to allocate a smaller buffer, which can fail: the map
    // must stay valid.
    std::size_t nb_erase_failures = 0;
    for (const std::string& key : make_keys<std::string>(300)) {
      ar.fail_after = 0;
      try {
        if (map.erase(key) == 1) {
          model.erase(key);
        }
      } catch (const std::bad_alloc&) {
        nb_erase_failures++;
      }
      ar.fail_after = -1;
    }
    BOOST_CHECK(nb_erase_failures > 0);
    check_matches(map, model);
  }

  check_arena_is_clean(ar);
}

/**
 * array_hash: the layer below the trie nodes, used on its own.
 */
BOOST_AUTO_TEST_CASE(test_array_map_and_set) {
  using alloc_type = arena_allocator<char, false>;
  using amap =
      tsl::array_map<char, int, tsl::ah::str_hash<char>,
                     tsl::ah::str_equal<char>, true, std::uint16_t,
                     std::uint32_t, tsl::ah::power_of_two_growth_policy<2>,
                     alloc_type>;
  using aset =
      tsl::array_set<char, tsl::ah::str_hash<char>, tsl::ah::str_equal<char>,
                     true, std::uint16_t, std::uint32_t,
                     tsl::ah::power_of_two_growth_policy<2>, alloc_type>;

  arena ar, ar2;
  {
    const alloc_type alloc(ar);
    amap map(alloc);
    aset set(alloc);
    std::map<std::string, int> model;
    int counter = 0;
    for (const std::string& key : make_keys<std::string>(5000)) {
      if (key.empty()) {
        continue;
      }
      map.insert(key, counter);
      set.insert(key);
      model.emplace(key, counter);
      counter++;
    }

    BOOST_CHECK(map.get_allocator() == alloc_type(ar));
    BOOST_CHECK_EQUAL(map.size(), model.size());
    BOOST_CHECK_EQUAL(set.size(), model.size());
    for (const auto& kv : model) {
      BOOST_CHECK_EQUAL(map.at(kv.first), kv.second);
      BOOST_CHECK(set.count(kv.first) == 1);
    }

    // Erase, shrink, rehash.
    for (auto it = model.begin(); it != model.end();) {
      if (it->second % 3 == 0) {
        BOOST_CHECK_EQUAL(map.erase(it->first), 1u);
        BOOST_CHECK_EQUAL(set.erase(it->first), 1u);
        it = model.erase(it);
      } else {
        ++it;
      }
    }
    map.shrink_to_fit();
    set.shrink_to_fit();
    map.rehash(map.bucket_count() * 4);
    set.rehash(set.bucket_count() * 4);
    for (const auto& kv : model) {
      BOOST_CHECK_EQUAL(map.at(kv.first), kv.second);
      BOOST_CHECK(set.count(kv.first) == 1);
    }

    // Copy/move/swap.
    amap copy(map, alloc_type(ar2));
    BOOST_CHECK(copy.get_allocator() == alloc_type(ar2));
    amap moved(std::move(copy), alloc_type(ar));  // not equal: elements moved
    BOOST_CHECK(moved.get_allocator() == alloc_type(ar));
    BOOST_CHECK_EQUAL(moved.size(), model.size());
    moved.swap(map);
    for (const auto& kv : model) {
      BOOST_CHECK_EQUAL(map.at(kv.first), kv.second);
    }

    // Serialization.
    serializer serial;
    map.serialize(serial);
    deserializer dserial(serial.str());
    amap deserialized = amap::deserialize(dserial, true, alloc_type(ar2));
    BOOST_CHECK(deserialized.get_allocator() == alloc_type(ar2));
    BOOST_CHECK_EQUAL(deserialized.size(), model.size());
    for (const auto& kv : model) {
      BOOST_CHECK_EQUAL(deserialized.at(kv.first), kv.second);
    }
  }

  check_arena_is_clean(ar);
  check_arena_is_clean(ar2);
}

BOOST_AUTO_TEST_SUITE_END()

/* ===========================================================================
 * Boost.Interprocess: fancy pointers, relocatable structures
 * ===========================================================================
 */

template <class Segment>
using bip_allocator = bip::allocator<char, typename Segment::segment_manager>;

template <class Segment, class CharT, class T, class Policy>
using bip_map =
    tsl::htrie_map<CharT, T, tsl::detail_htrie_hash::default_hash<CharT>,
                   std::uint16_t, Policy, bip_allocator<Segment>>;

/**
 * Segment memory which is always aligned the same way, so that a copy of the
 * bytes of a segment can be opened as a segment.
 */
class aligned_memory {
 public:
  explicit aligned_memory(std::size_t size)
      : m_storage(size + 4096), m_size(size) {
    const std::uintptr_t address =
        reinterpret_cast<std::uintptr_t>(m_storage.data());
    m_ptr = m_storage.data() + ((4096 - address % 4096) % 4096);
  }

  void* data() const { return m_ptr; }
  std::size_t size() const { return m_size; }

 private:
  std::vector<char> m_storage;
  std::size_t m_size;
  char* m_ptr;
};

using external_segment = bip::managed_external_buffer;

BOOST_AUTO_TEST_SUITE(test_htrie_interprocess)

/**
 * Build a map in a segment, copy the segment's bytes elsewhere, destroy the
 * original bytes, then use the map from the copy: there must be no pointer to
 * the original address left in the structure.
 */
template <class P, class CharT>
void test_relocation(std::size_t burst_threshold) {
  using map_type = bip_map<external_segment, CharT, std::int64_t, P>;
  using key_type = typename map_type::key_buffer_type;
  using hash_type = tsl::detail_htrie_hash::default_hash<CharT>;

  const std::size_t segment_size = 16 * 1024 * 1024;
  aligned_memory original(segment_size);
  aligned_memory moved(segment_size);

  model_of<map_type> model;
  {
    external_segment segment(bip::create_only, original.data(), segment_size);
    map_type* map = segment.construct<map_type>("map")(
        burst_threshold, hash_type(),
        bip_allocator<external_segment>(segment.get_segment_manager()));

    fill(*map, model, 6000);
    check_matches(*map, model);
    erase_every_other(*map, model);
    check_matches(*map, model);
    BOOST_CHECK(segment.get_free_memory() < segment_size);

    // Everything the map uses is in the segment: copy it.
    std::memcpy(moved.data(), original.data(), segment_size);
    // Poison the original memory (the segment is not used from now on).
    std::memset(original.data(), 0xDE, segment_size);
  }

  {
    external_segment segment(bip::open_only, moved.data(), segment_size);
    map_type* map = segment.find<map_type>("map").first;
    BOOST_REQUIRE(map != nullptr);
    BOOST_CHECK(static_cast<void*>(map) != original.data());
    check_matches(*map, model);

    // The relocated map is fully functional.
    std::int64_t counter = 100000;
    for (const key_type& key : make_keys<key_type>(4000, 99)) {
      if (map->insert(key, counter).second) {
        model.emplace(key, counter);
        counter++;
      }
    }
    check_matches(*map, model);

    erase_every_other(*map, model);
    map->shrink_to_fit();
    check_matches(*map, model);

    // Copy inside the segment (the allocator is copied: same segment).
    map_type* copy = segment.construct<map_type>("copy")(*map);
    check_matches(*copy, model);
    segment.destroy_ptr(copy);

    // Serialization round trip, in the segment.
    serializer serial;
    map->serialize(serial);
    deserializer dserial(serial.str());
    map_type* deserialized = segment.construct<map_type>("deserialized")(
        burst_threshold, hash_type(),
        bip_allocator<external_segment>(segment.get_segment_manager()));
    *deserialized = map_type::deserialize(
        dserial, false,
        bip_allocator<external_segment>(segment.get_segment_manager()));
    check_matches(*deserialized, model);
    segment.destroy_ptr(deserialized);

    // Everything is given back to the segment.
    map->clear();
    segment.destroy_ptr(map);
    BOOST_CHECK(segment.all_memory_deallocated());
    BOOST_CHECK(segment.check_sanity());
  }
}

BOOST_AUTO_TEST_CASE(test_relocation_dense_array) {
  test_relocation<pol::dense_array_children, char>(8);
  test_relocation<pol::dense_array_children, char>(1024);
}

BOOST_AUTO_TEST_CASE(test_relocation_flat_map) {
  test_relocation<pol::flat_map_children, char>(8);
  test_relocation<pol::flat_map_children, char>(1024);
}

BOOST_AUTO_TEST_CASE(test_relocation_bitmap) {
  test_relocation<pol::bitmap_children, char>(8);
  test_relocation<pol::bitmap_children, char>(1024);
}

BOOST_AUTO_TEST_CASE(test_relocation_wide_letters) {
  test_relocation<pol::flat_map_children, char32_t>(8);
}

BOOST_AUTO_TEST_CASE(test_set_relocation) {
  using set_type =
      tsl::htrie_set<char, hash_char, std::uint16_t, pol::flat_map_children,
                     bip_allocator<external_segment>>;

  const std::size_t segment_size = 4 * 1024 * 1024;
  aligned_memory original(segment_size);
  aligned_memory moved(segment_size);

  std::map<std::string, int> model;
  {
    external_segment segment(bip::create_only, original.data(), segment_size);
    set_type* set = segment.construct<set_type>("set")(
        std::size_t(8), hash_char(),
        bip_allocator<external_segment>(segment.get_segment_manager()));
    for (const std::string& key : make_keys<std::string>(3000)) {
      BOOST_CHECK_EQUAL(set->insert(key).second, model.emplace(key, 0).second);
    }
    std::memcpy(moved.data(), original.data(), segment_size);
    std::memset(original.data(), 0xDE, segment_size);
  }

  external_segment segment(bip::open_only, moved.data(), segment_size);
  set_type* set = segment.find<set_type>("set").first;
  BOOST_REQUIRE(set != nullptr);
  check_set_matches(*set, model);
  segment.destroy_ptr(set);
  BOOST_CHECK(segment.all_memory_deallocated());
}

BOOST_AUTO_TEST_CASE(test_values_that_use_the_segment) {
  using string_type = bip::basic_string<
      char, std::char_traits<char>,
      bip::allocator<char, external_segment::segment_manager>>;
  using map_type =
      bip_map<external_segment, char, string_type, pol::flat_map_children>;

  const std::size_t segment_size = 8 * 1024 * 1024;
  aligned_memory original(segment_size);
  aligned_memory moved(segment_size);

  std::map<std::string, std::string> model;
  {
    external_segment segment(bip::create_only, original.data(), segment_size);
    external_segment::segment_manager* manager = segment.get_segment_manager();

    map_type* map = segment.construct<map_type>("map")(
        std::size_t(16), hash_char(), bip_allocator<external_segment>(manager));

    std::size_t i = 0;
    for (const std::string& key : make_keys<std::string>(2500)) {
      // The values are built with the allocator of the segment by the caller
      // (long enough to not be stored inline in the string).
      const std::string value = "value of the key " + std::to_string(i++) +
                                " (a long string, stored in the segment)";
      if (map->emplace(key, string_type(value.c_str(), manager)).second) {
        model.emplace(key, value);
      }
    }

    std::memcpy(moved.data(), original.data(), segment_size);
    std::memset(original.data(), 0xDE, segment_size);
  }

  external_segment segment(bip::open_only, moved.data(), segment_size);
  map_type* map = segment.find<map_type>("map").first;
  BOOST_REQUIRE(map != nullptr);

  BOOST_CHECK_EQUAL(map->size(), model.size());
  for (const auto& kv : model) {
    auto it = map->find(kv.first);
    BOOST_REQUIRE(it != map->end());
    BOOST_CHECK(std::string(it.value().c_str()) == kv.second);
  }

  // Erasing destroys the strings in the segment.
  for (const auto& kv : model) {
    BOOST_CHECK_EQUAL(map->erase(kv.first), 1u);
  }
  BOOST_CHECK(map->empty());
  segment.destroy_ptr(map);
  BOOST_CHECK(segment.all_memory_deallocated());
}

/**
 * A full segment: allocations fail with Boost.Interprocess' exception, the map
 * stays consistent and gives all its memory back.
 */
BOOST_AUTO_TEST_CASE(test_full_segment) {
  using map_type =
      bip_map<external_segment, char, std::int64_t, pol::flat_map_children>;

  const std::size_t segment_size = 256 * 1024;
  aligned_memory memory(segment_size);

  external_segment segment(bip::create_only, memory.data(), segment_size);
  map_type* map = segment.construct<map_type>("map")(
      std::size_t(8), hash_char(),
      bip_allocator<external_segment>(segment.get_segment_manager()));

  model_of<map_type> model;
  bool full = false;
  std::int64_t counter = 0;
  for (const std::string& key : make_keys<std::string>(200000, 5)) {
    try {
      if (map->insert(key, counter).second) {
        model.emplace(key, counter);
      }
      counter++;
    } catch (const bip::bad_alloc&) {
      full = true;
      break;
    }
  }

  BOOST_REQUIRE(full);
  BOOST_CHECK(model.size() > 1000);
  check_matches(*map, model);

  map->clear();
  segment.destroy_ptr(map);
  BOOST_CHECK(segment.all_memory_deallocated());
}

/**
 * Short enough for the names of the POSIX shared memory objects (31 characters
 * on macOS).
 */
std::string unique_name(const char* prefix) {
  const unsigned long long ticks = static_cast<unsigned long long>(
      std::chrono::steady_clock::now().time_since_epoch().count());
  return std::string(prefix) + std::to_string(ticks % 1000000000ULL);
}

std::string temp_directory() {
  for (const char* variable : {"TMPDIR", "TMP", "TEMP"}) {
    const char* value = std::getenv(variable);
    if (value != nullptr && *value != '\0') {
      return value;
    }
  }

  return "/tmp";
}

/**
 * The same file mapped twice in the process: two different addresses at the
 * same time, changes made through one are seen through the other.
 */
BOOST_AUTO_TEST_CASE(test_memory_mapped_file_mapped_twice) {
  using segment_type = bip::managed_mapped_file;
  using map_type =
      bip_map<segment_type, char, std::int64_t, pol::flat_map_children>;

  const std::string path =
      temp_directory() + "/" + unique_name("tsl_htrie_test_");
  bip::file_mapping::remove(path.c_str());

  const std::size_t file_size = 16 * 1024 * 1024;
  model_of<map_type> model;
  try {
    segment_type first(bip::create_only, path.c_str(), file_size);
    map_type* map1 = first.construct<map_type>("map")(
        std::size_t(8), hash_char(),
        bip_allocator<segment_type>(first.get_segment_manager()));
    fill(*map1, model, 5000);

    segment_type second(bip::open_only, path.c_str());
    BOOST_REQUIRE(first.get_address() != second.get_address());

    map_type* map2 = second.find<map_type>("map").first;
    BOOST_REQUIRE(map2 != nullptr);
    BOOST_CHECK(static_cast<void*>(map1) != static_cast<void*>(map2));
    check_matches(*map2, model);

    // A change through the second mapping is visible through the first.
    for (const auto& kv : model) {
      map2->insert(kv.first + "_added", kv.second);
    }
    for (const auto& kv : std::map<std::string, std::int64_t>(model)) {
      model.emplace(kv.first + "_added", kv.second);
    }
    check_matches(*map1, model);

    erase_every_other(*map1, model);
    check_matches(*map2, model);

    first.destroy_ptr(map1);
    BOOST_CHECK(first.all_memory_deallocated());
  } catch (...) {
    bip::file_mapping::remove(path.c_str());
    throw;
  }

  bip::file_mapping::remove(path.c_str());
}

/**
 * The file is closed and opened later: the data is still there.
 */
BOOST_AUTO_TEST_CASE(test_memory_mapped_file_reopened) {
  using segment_type = bip::managed_mapped_file;
  using map_type =
      bip_map<segment_type, char, std::int64_t, pol::bitmap_children>;

  const std::string path =
      temp_directory() + "/" + unique_name("tsl_htrie_reopen_");
  bip::file_mapping::remove(path.c_str());

  model_of<map_type> model;
  try {
    {
      segment_type segment(bip::create_only, path.c_str(), 8 * 1024 * 1024);
      map_type* map = segment.construct<map_type>("map")(
          std::size_t(16), hash_char(),
          bip_allocator<segment_type>(segment.get_segment_manager()));
      fill(*map, model, 4000);
      segment.flush();
    }

    {
      segment_type segment(bip::open_only, path.c_str());
      map_type* map = segment.find<map_type>("map").first;
      BOOST_REQUIRE(map != nullptr);
      check_matches(*map, model);

      erase_every_other(*map, model);
    }

    {
      // Open for the changes (and grow the file, the map is still valid).
      segment_type::grow(path.c_str(), 4 * 1024 * 1024);
      segment_type segment(bip::open_only, path.c_str());
      map_type* map = segment.find<map_type>("map").first;
      BOOST_REQUIRE(map != nullptr);
      check_matches(*map, model);

      std::int64_t counter = 1 << 20;
      for (const std::string& key : make_keys<std::string>(3000, 17)) {
        if (map->insert(key, counter).second) {
          model.emplace(key, counter);
          counter++;
        }
      }
      check_matches(*map, model);

      segment.destroy_ptr(map);
      BOOST_CHECK(segment.all_memory_deallocated());
    }
  } catch (...) {
    bip::file_mapping::remove(path.c_str());
    throw;
  }

  bip::file_mapping::remove(path.c_str());
}

#ifndef _WIN32
/**
 * Shared memory objects (POSIX shared memory), mapped twice.
 */
BOOST_AUTO_TEST_CASE(test_shared_memory) {
  using segment_type = bip::managed_shared_memory;
  using map_type =
      bip_map<segment_type, char, std::int64_t, pol::dense_array_children>;

  struct shm_remover {
    explicit shm_remover(std::string shm_name) : name(std::move(shm_name)) {
      bip::shared_memory_object::remove(name.c_str());
    }
    ~shm_remover() { bip::shared_memory_object::remove(name.c_str()); }

    std::string name;
  } remover(unique_name("tsl_htrie_shm_"));

  std::unique_ptr<segment_type> first;
  try {
    first.reset(new segment_type(bip::create_only, remover.name.c_str(),
                                 16 * 1024 * 1024));
  } catch (const bip::interprocess_exception& ex) {
    // Shared memory may not be available (restricted environments): don't
    // fail the tests for that, the file and buffer tests cover the logic.
    BOOST_TEST_MESSAGE(std::string("shared memory not available: ") +
                       ex.what());
    return;
  }

  map_type* map1 = first->construct<map_type>("map")(
      std::size_t(32), hash_char(),
      bip_allocator<segment_type>(first->get_segment_manager()));
  model_of<map_type> model;
  fill(*map1, model, 5000);

  segment_type second(bip::open_only, remover.name.c_str());
  BOOST_REQUIRE(first->get_address() != second.get_address());
  map_type* map2 = second.find<map_type>("map").first;
  BOOST_REQUIRE(map2 != nullptr);
  check_matches(*map2, model);

  erase_every_other(*map2, model);
  check_matches(*map1, model);

  first->destroy_ptr(map1);
  BOOST_CHECK(first->all_memory_deallocated());
}
#endif  // _WIN32

BOOST_AUTO_TEST_SUITE_END()

}  // namespace
