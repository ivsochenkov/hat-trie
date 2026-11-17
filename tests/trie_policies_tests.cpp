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
 * Tests of the generic key (any integral letter type, any contiguous sequence
 * of letters as key), of the children policies and of the allocator parameter.
 */
#include <algorithm>
#include <array>
#include <boost/mpl/list.hpp>
#include <boost/test/unit_test.hpp>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <random>
#include <set>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "tsl/htrie_map.h"
#include "tsl/htrie_set.h"
#include "utils.h"

namespace {

namespace pol = tsl::htrie_policy;

/**
 * Stateless allocator counting its live allocations.
 */
struct allocation_counter {
  static long& live() {
    static long value = 0;
    return value;
  }

  static long& total() {
    static long value = 0;
    return value;
  }
};

template <class T>
struct counting_allocator {
  using value_type = T;

  counting_allocator() = default;
  template <class U>
  counting_allocator(const counting_allocator<U>&) noexcept {}

  T* allocate(std::size_t n) {
    ++allocation_counter::live();
    ++allocation_counter::total();
    return std::allocator<T>().allocate(n);
  }

  void deallocate(T* ptr, std::size_t n) noexcept {
    --allocation_counter::live();
    std::allocator<T>().deallocate(ptr, n);
  }

  template <class U>
  bool operator==(const counting_allocator<U>&) const noexcept {
    return true;
  }

  template <class U>
  bool operator!=(const counting_allocator<U>&) const noexcept {
    return false;
  }
};

template <class CharT, class Policy>
using test_map = tsl::htrie_map<CharT, std::int64_t,
                                tsl::detail_htrie_hash::default_hash<CharT>,
                                std::uint16_t, Policy>;

template <class CharT, class Policy>
using test_set =
    tsl::htrie_set<CharT, tsl::detail_htrie_hash::default_hash<CharT>,
                   std::uint16_t, Policy>;

template <class CharT, class Policy>
using test_alloc_map =
    tsl::htrie_map<CharT, std::int64_t,
                   tsl::detail_htrie_hash::default_hash<CharT>, std::uint16_t,
                   Policy, counting_allocator<char>>;

// Every (letter type, policy) pair that is allowed.
using map_types =
    boost::mpl::list<test_map<char, pol::dense_array_children>,
                     test_map<char, pol::flat_map_children>,
                     test_map<char, pol::bitmap_children>,
                     test_map<char, pol::std_map_children>,
                     test_map<unsigned char, pol::bitmap_children>,
                     test_map<std::uint16_t, pol::dense_array_children>,
                     test_map<char16_t, pol::flat_map_children>,
                     test_map<wchar_t, pol::flat_map_children>,
                     test_map<wchar_t, pol::std_map_children>,
                     test_map<char32_t, pol::flat_map_children>,
                     test_map<unsigned int, pol::flat_map_children>,
                     test_map<std::int32_t, pol::std_map_children>,
                     test_map<std::uint64_t, pol::flat_map_children>>;

using alloc_map_types =
    boost::mpl::list<test_alloc_map<char, pol::dense_array_children>,
                     test_alloc_map<char, pol::flat_map_children>,
                     test_alloc_map<char, pol::bitmap_children>,
                     test_alloc_map<char, pol::std_map_children>,
                     test_alloc_map<char32_t, pol::flat_map_children>,
                     test_alloc_map<unsigned int, pol::std_map_children>>;

using set_types =
    boost::mpl::list<test_set<char, pol::bitmap_children>,
                     test_set<char, pol::dense_array_children>,
                     test_set<char16_t, pol::flat_map_children>,
                     test_set<char32_t, pol::flat_map_children>,
                     test_set<unsigned int, pol::flat_map_children>,
                     test_set<std::uint64_t, pol::std_map_children>>;

/**
 * Letters used to build keys: a few dense small values (to get long shared
 * prefixes and lots of bursting) and values spread over the whole range of the
 * type (including the sign bit, the 8/16/32 bits boundaries and 0).
 */
template <class CharT>
CharT random_letter(std::mt19937& rng, unsigned alphabet) {
  static const std::uint64_t spread[] = {
      0u,          1u,          127u,           128u,
      255u,        256u,        0x7fffu,        0x8000u,
      0xffffu,     0x10000u,    0x1f600u,       0x7fffffffu,
      0x80000000u, 0xffffffffu, 0x100000000ull, 0xfedcba9876543210ull};

  const unsigned r = rng() % 100;
  if (r < 75) {
    return static_cast<CharT>(rng() % alphabet);
  } else {
    return static_cast<CharT>(
        spread[rng() % (sizeof(spread) / sizeof(spread[0]))]);
  }
}

template <class CharT>
std::vector<CharT> random_key(std::mt19937& rng, unsigned alphabet,
                              std::size_t max_size) {
  std::vector<CharT> key(rng() % (max_size + 1));
  for (CharT& letter : key) {
    letter = random_letter<CharT>(rng, alphabet);
  }

  return key;
}

template <class Key>
bool has_prefix(const Key& key, const Key& prefix) {
  return key.size() >= prefix.size() &&
         std::equal(prefix.begin(), prefix.end(), key.begin());
}

template <class It>
auto key_vector(const It& it)
    -> std::vector<typename decltype(it.key())::value_type> {
  const auto key = it.key();
  return std::vector<typename decltype(it.key())::value_type>(key.begin(),
                                                              key.end());
}

template <class TMap>
std::vector<std::vector<typename TMap::char_type>> all_keys(const TMap& map) {
  std::vector<std::vector<typename TMap::char_type>> keys;
  for (auto it = map.cbegin(); it != map.cend(); ++it) {
    keys.push_back(key_vector(it));
  }

  return keys;
}

/**
 * Compare the content of the trie with the reference (keys and values).
 */
template <class TMap, class Reference>
void check_map_content(const TMap& map, const Reference& reference) {
  using key_tt = typename Reference::key_type;

  BOOST_REQUIRE_EQUAL(map.size(), reference.size());

  std::set<key_tt> iterated;
  for (auto it = map.cbegin(); it != map.cend(); ++it) {
    const key_tt key = key_vector(it);

    const auto it_ref = reference.find(key);
    BOOST_REQUIRE(it_ref != reference.end());
    BOOST_CHECK_EQUAL(it.value(), it_ref->second);
    BOOST_CHECK(iterated.insert(key).second);
  }
  BOOST_CHECK_EQUAL(iterated.size(), reference.size());

  for (const auto& kv : reference) {
    const auto it = map.find(kv.first);
    BOOST_REQUIRE(it != map.cend());
    BOOST_CHECK_EQUAL(it.value(), kv.second);
    BOOST_CHECK_EQUAL(map.count(kv.first), 1u);
  }
}

}  // namespace

BOOST_AUTO_TEST_SUITE(test_htrie_generic_keys)

/*
 * Types
 */
BOOST_AUTO_TEST_CASE(test_key_buffer_type) {
  static_assert(std::is_same<tsl::htrie_map<char, int>::key_buffer_type,
                             std::string>::value,
                "");
  static_assert(std::is_same<tsl::htrie_map<wchar_t, int>::key_buffer_type,
                             std::wstring>::value,
                "");
  static_assert(std::is_same<tsl::htrie_map<char16_t, int>::key_buffer_type,
                             std::u16string>::value,
                "");
  static_assert(std::is_same<tsl::htrie_map<char32_t, int>::key_buffer_type,
                             std::u32string>::value,
                "");
  static_assert(std::is_same<tsl::htrie_map<unsigned int, int>::key_buffer_type,
                             std::vector<unsigned int>>::value,
                "");
  static_assert(std::is_same<tsl::htrie_set<std::uint16_t>::key_buffer_type,
                             std::vector<std::uint16_t>>::value,
                "");

  static_assert(std::is_same<tsl::htrie_map<char, int>::children_policy,
                             pol::dense_array_children>::value,
                "default policy of 8 bits letters is the dense array");
  static_assert(std::is_same<tsl::htrie_map<char32_t, int>::children_policy,
                             pol::flat_map_children>::value,
                "default policy of wider letters is the flat map");
  BOOST_CHECK(true);
}

/*
 * Keys given in every supported form
 */
BOOST_AUTO_TEST_CASE(test_key_forms_u32) {
  tsl::htrie_map<char32_t, int> map;

  // literals (null-terminated arrays)
  map.insert(U"\U0001F600 smile", 1);
  map[U"€ euro"] = 2;
  map.emplace(U"Жук", 3);  // Cyrillic

  // std::u32string, std::u32string_view-like and std::vector
  map.insert(std::u32string(U"string"), 4);
  map.insert(std::vector<char32_t>{U'v', U'e', U'c'}, 5);
  map.insert(std::array<char32_t, 3>{{U'a', U'r', U'r'}}, 6);

  // pointer + size
  const char32_t raw[] = {0x10FFFF, 0x0, 0x1, 0x10FFFF};
  map.insert_ks(raw, 4, 7);  // contains a null letter in the middle

  BOOST_CHECK_EQUAL(map.size(), 7u);

  BOOST_CHECK_EQUAL(map.at(U"\U0001F600 smile"), 1);
  BOOST_CHECK_EQUAL(map.at(std::u32string(U"€ euro")), 2);
  BOOST_CHECK_EQUAL(map.at(std::vector<char32_t>{0x416, 0x443, 0x43A}), 3);
  BOOST_CHECK_EQUAL(map.at(U"string"), 4);
  BOOST_CHECK_EQUAL(map.at(U"vec"), 5);
  BOOST_CHECK_EQUAL(map.at(U"arr"), 6);
  BOOST_CHECK_EQUAL(map.at_ks(raw, 4), 7);
  BOOST_CHECK(map.find_ks(raw, 3) == map.end());
  BOOST_CHECK_THROW(map.at(U"unknown"), std::out_of_range);

  // The key of an iterator is a std::u32string
  const auto it = map.find(U"€ euro");
  BOOST_REQUIRE(it != map.end());
  BOOST_CHECK(it.key() == std::u32string(U"€ euro"));

  // The key with the null letter is not truncated
  const auto it_raw = map.find_ks(raw, 4);
  BOOST_REQUIRE(it_raw != map.end());
  BOOST_CHECK(it_raw.key() == std::u32string(raw, raw + 4));

  BOOST_CHECK_EQUAL(map.erase(U"vec"), 1u);
  BOOST_CHECK_EQUAL(map.erase(U"vec"), 0u);
  BOOST_CHECK_EQUAL(map.erase_ks(raw, 4), 1u);
  BOOST_CHECK_EQUAL(map.size(), 5u);
}

BOOST_AUTO_TEST_CASE(test_key_forms_wchar) {
  tsl::htrie_map<wchar_t, int> map = {{L"héllo", 1}, {L"world", 2}};
  map.insert(std::wstring(L"wörld"), 3);

  BOOST_CHECK_EQUAL(map.size(), 3u);
  BOOST_CHECK_EQUAL(map.at(L"héllo"), 1);
  BOOST_CHECK_EQUAL(map.at(std::wstring(L"world")), 2);
  BOOST_CHECK_EQUAL(map.at(L"wörld"), 3);
  BOOST_CHECK(map.find(L"hello") == map.end());

  tsl::htrie_set<char16_t> set = {u"العربية", u"日本語"};
  BOOST_CHECK_EQUAL(set.size(), 2u);
  BOOST_CHECK(set.find(u"日本語") != set.end());
  BOOST_CHECK_EQUAL(set.count(u"日本"), 0u);
}

BOOST_AUTO_TEST_CASE(test_key_forms_integer_vector) {
  // Any sequence of integers can be a key
  tsl::htrie_map<unsigned int, std::string> map;

  const std::vector<unsigned int> k1 = {1, 2, 3};
  const std::vector<unsigned int> k2 = {1, 2, 3, 4000000000u};
  const std::vector<unsigned int> k3 = {4000000000u};
  const std::vector<unsigned int> empty;

  map.insert(k1, "k1");
  map.insert(k2, "k2");
  map.insert(k3, "k3");
  map.insert(empty, "empty");  // empty vector: data() may be null

  BOOST_CHECK_EQUAL(map.size(), 4u);
  BOOST_CHECK_EQUAL(map.at(k1), "k1");
  BOOST_CHECK_EQUAL(map.at(k2), "k2");
  BOOST_CHECK_EQUAL(map.at(k3), "k3");
  BOOST_CHECK_EQUAL(map.at(empty), "empty");
  BOOST_CHECK_EQUAL(map.at(std::vector<unsigned int>()), "empty");

  const unsigned int zero_terminated[] = {1, 2, 3, 0};
  BOOST_CHECK_EQUAL(map.at(static_cast<const unsigned int*>(zero_terminated)),
                    "k1");

  // longest_prefix / prefix range on non-string keys
  const std::vector<unsigned int> query = {1, 2, 3, 4000000000u, 7};
  const auto it_longest = map.longest_prefix(query);
  BOOST_REQUIRE(it_longest != map.end());
  BOOST_CHECK(it_longest.key() == k2);

  std::set<std::vector<unsigned int>> found;
  map.for_each_prefix_of(
      query, [&](decltype(map)::const_iterator it) { found.insert(it.key()); });
  BOOST_CHECK(found == (std::set<std::vector<unsigned int>>{empty, k1, k2}));

  auto range = map.equal_prefix_range(std::vector<unsigned int>{1, 2});
  std::set<std::vector<unsigned int>> in_range;
  for (auto it = range.first; it != range.second; ++it) {
    in_range.insert(it.key());
  }
  BOOST_CHECK(in_range == (std::set<std::vector<unsigned int>>{k1, k2}));

  BOOST_CHECK_EQUAL(map.erase_prefix(std::vector<unsigned int>{1}), 2u);
  BOOST_CHECK_EQUAL(map.size(), 2u);
}

BOOST_AUTO_TEST_CASE(test_too_long_wide_key) {
  tsl::htrie_map<std::uint32_t, int> map;

  std::vector<std::uint32_t> key(map.max_key_size() + 1, 0xdeadbeefu);
  BOOST_CHECK_THROW(map.insert(key, 1), std::length_error);

  key.pop_back();
  BOOST_CHECK(map.insert(key, 1).second);
  BOOST_CHECK_EQUAL(map.at(key), 1);
}

/*
 * Compare against a reference container, for every letter type / policy
 */
BOOST_AUTO_TEST_CASE_TEMPLATE(test_map_random_operations, TMap, map_types) {
  using char_tt = typename TMap::char_type;
  using key_tt = std::vector<char_tt>;

  const unsigned alphabets[] = {3, 30, 300};
  const std::size_t burst_thresholds[] = {4, 16, 64};

  for (unsigned seed = 1; seed <= 3; seed++) {
    for (std::size_t iconfig = 0; iconfig < 3; iconfig++) {
      std::mt19937 rng(seed * 7919 + static_cast<unsigned>(iconfig));
      const unsigned alphabet = alphabets[iconfig];

      TMap map(burst_thresholds[iconfig]);
      std::map<key_tt, std::int64_t> reference;
      std::vector<key_tt> inserted_keys;

      for (std::int64_t op = 0; op < 2500; op++) {
        const unsigned action = rng() % 100;
        if (action < 55 || inserted_keys.empty()) {
          const key_tt key = random_key<char_tt>(rng, alphabet, 8);
          const auto result = map.insert(key, op);

          BOOST_CHECK_EQUAL(result.second, reference.insert({key, op}).second);
          BOOST_CHECK(key_vector(result.first) == key);
          BOOST_CHECK_EQUAL(result.first.value(), reference.at(key));
          inserted_keys.push_back(key);
        } else if (action < 80) {
          const key_tt& key = inserted_keys[rng() % inserted_keys.size()];
          BOOST_CHECK_EQUAL(map.erase(key), reference.erase(key));
        } else if (action < 95) {
          const key_tt key = (rng() % 2)
                                 ? inserted_keys[rng() % inserted_keys.size()]
                                 : random_key<char_tt>(rng, alphabet, 8);
          BOOST_CHECK_EQUAL(map.count(key), reference.count(key));
        } else {
          key_tt prefix = inserted_keys[rng() % inserted_keys.size()];
          prefix.resize(rng() % (prefix.size() + 1));

          std::size_t nb_erased = 0;
          for (auto it = reference.begin(); it != reference.end();) {
            if (has_prefix(it->first, prefix)) {
              it = reference.erase(it);
              nb_erased++;
            } else {
              ++it;
            }
          }

          BOOST_CHECK_EQUAL(map.erase_prefix(prefix), nb_erased);
        }
      }

      check_map_content(map, reference);

      // copy / move / assign
      TMap copy(map);
      check_map_content(copy, reference);
      BOOST_CHECK(copy == map);

      TMap moved(std::move(copy));
      check_map_content(moved, reference);
      BOOST_CHECK(copy.empty());

      TMap assigned;
      assigned = map;
      check_map_content(assigned, reference);

      // shrink_to_fit must keep the content
      moved.shrink_to_fit();
      check_map_content(moved, reference);

      moved.clear();
      BOOST_CHECK(moved.empty());
      BOOST_CHECK(moved.begin() == moved.end());
    }
  }
}

BOOST_AUTO_TEST_CASE_TEMPLATE(test_map_prefix_operations, TMap, map_types) {
  using char_tt = typename TMap::char_type;
  using key_tt = std::vector<char_tt>;

  std::mt19937 rng(42);

  TMap map(8);
  std::map<key_tt, std::int64_t> reference;
  for (std::int64_t i = 0; i < 1500; i++) {
    const key_tt key = random_key<char_tt>(rng, 6, 9);
    map.insert(key, i);
    reference.insert({key, i});
  }

  for (std::size_t iquery = 0; iquery < 200; iquery++) {
    // prefix range
    key_tt prefix = random_key<char_tt>(rng, 6, 4);

    std::set<key_tt> expected;
    for (const auto& kv : reference) {
      if (has_prefix(kv.first, prefix)) {
        expected.insert(kv.first);
      }
    }

    std::set<key_tt> got;
    const TMap& const_map = map;
    const auto range = const_map.equal_prefix_range(prefix);
    for (auto it = range.first; it != range.second; ++it) {
      BOOST_CHECK(got.insert(key_vector(it)).second);
    }
    BOOST_CHECK(got == expected);

    // longest_prefix and for_each_prefix_of
    const key_tt query = random_key<char_tt>(rng, 6, 12);

    std::set<key_tt> expected_prefixes;
    for (const auto& kv : reference) {
      if (has_prefix(query, kv.first)) {
        expected_prefixes.insert(kv.first);
      }
    }

    std::set<key_tt> visited;
    const_map.for_each_prefix_of(query, [&](typename TMap::const_iterator it) {
      visited.insert(key_vector(it));
    });
    BOOST_CHECK(visited == expected_prefixes);

    const auto it_longest = const_map.longest_prefix(query);
    if (expected_prefixes.empty()) {
      BOOST_CHECK(it_longest == const_map.cend());
    } else {
      BOOST_REQUIRE(it_longest != const_map.cend());
      const key_tt longest = key_vector(it_longest);
      for (const key_tt& other : expected_prefixes) {
        BOOST_CHECK(other.size() <= longest.size());
      }
      BOOST_CHECK(expected_prefixes.count(longest) == 1);
    }
  }
}

BOOST_AUTO_TEST_CASE_TEMPLATE(test_map_serialize_deserialize, TMap, map_types) {
  using char_tt = typename TMap::char_type;
  using key_tt = std::vector<char_tt>;

  std::mt19937 rng(7);

  TMap map(8);
  std::map<key_tt, std::int64_t> reference;
  for (std::int64_t i = 0; i < 1500; i++) {
    const key_tt key = random_key<char_tt>(rng, 10, 8);
    map.insert(key, i);
    reference.insert({key, i});
  }

  for (const bool hash_compatible : {false, true}) {
    serializer serial;
    map.serialize(serial);

    deserializer deserial(serial.str());
    const TMap deserialized = TMap::deserialize(deserial, hash_compatible);

    check_map_content(deserialized, reference);
    BOOST_CHECK(deserialized == map);
  }
}

BOOST_AUTO_TEST_CASE_TEMPLATE(test_set_random_operations, TSet, set_types) {
  using char_tt = typename TSet::char_type;
  using key_tt = std::vector<char_tt>;

  std::mt19937 rng(11);

  TSet set(8);
  std::set<key_tt> reference;
  std::vector<key_tt> inserted_keys;

  for (int op = 0; op < 4000; op++) {
    const unsigned action = rng() % 100;
    if (action < 60 || inserted_keys.empty()) {
      const key_tt key = random_key<char_tt>(rng, 12, 8);
      BOOST_CHECK_EQUAL(set.insert(key).second, reference.insert(key).second);
      inserted_keys.push_back(key);
    } else if (action < 85) {
      const key_tt& key = inserted_keys[rng() % inserted_keys.size()];
      BOOST_CHECK_EQUAL(set.erase(key), reference.erase(key));
    } else {
      key_tt prefix = inserted_keys[rng() % inserted_keys.size()];
      prefix.resize(rng() % (prefix.size() + 1));

      std::size_t nb_erased = 0;
      for (auto it = reference.begin(); it != reference.end();) {
        if (has_prefix(*it, prefix)) {
          it = reference.erase(it);
          nb_erased++;
        } else {
          ++it;
        }
      }
      BOOST_CHECK_EQUAL(set.erase_prefix(prefix), nb_erased);
    }
  }

  BOOST_REQUIRE_EQUAL(set.size(), reference.size());
  std::set<key_tt> iterated;
  for (auto it = set.cbegin(); it != set.cend(); ++it) {
    BOOST_CHECK(iterated.insert(key_vector(it)).second);
  }
  BOOST_CHECK(iterated == reference);

  TSet copy(set);
  BOOST_CHECK(copy == set);

  serializer serial;
  set.serialize(serial);
  deserializer deserial(serial.str());
  const TSet deserialized = TSet::deserialize(deserial);
  BOOST_CHECK(deserialized == set);
}

/*
 * The iteration order of the trie nodes must not depend on the policy
 */
BOOST_AUTO_TEST_CASE(test_same_iteration_order_for_all_policies) {
  std::mt19937 rng(2024);

  test_map<char, pol::dense_array_children> dense(8);
  test_map<char, pol::flat_map_children> flat(8);
  test_map<char, pol::bitmap_children> bitmap(8);
  test_map<char, pol::std_map_children> std_map(8);

  for (std::int64_t i = 0; i < 3000; i++) {
    // use the whole range of letters, signed chars included
    const auto key = random_key<char>(rng, 256, 6);
    dense.insert(key, i);
    flat.insert(key, i);
    bitmap.insert(key, i);
    std_map.insert(key, i);
  }

  const auto expected = all_keys(dense);
  BOOST_CHECK(!expected.empty());

  const auto flat_keys = all_keys(flat);
  const auto bitmap_keys = all_keys(bitmap);
  const auto map_keys = all_keys(std_map);

  BOOST_CHECK(flat_keys == expected);
  BOOST_CHECK(bitmap_keys == expected);
  BOOST_CHECK(map_keys == expected);
}

/*
 * Allocator
 */
BOOST_AUTO_TEST_CASE_TEMPLATE(test_allocator, TMap, alloc_map_types) {
  using char_tt = typename TMap::char_type;

  static_assert(std::is_same<typename TMap::allocator_type,
                             counting_allocator<char>>::value,
                "");

  const long live_before = allocation_counter::live();
  const long total_before = allocation_counter::total();

  {
    std::mt19937 rng(5);

    TMap map(8);
    for (std::int64_t i = 0; i < 2000; i++) {
      map.insert(random_key<char_tt>(rng, 20, 8), i);
    }

    BOOST_CHECK(allocation_counter::live() > live_before);
    BOOST_CHECK(allocation_counter::total() > total_before);

    TMap copy(map);
    BOOST_CHECK(copy == map);

    // erase a part of the elements, nodes are released
    const long live_full = allocation_counter::live();
    copy.erase_prefix(std::vector<char_tt>{static_cast<char_tt>(1)});
    copy.erase_prefix(std::vector<char_tt>{static_cast<char_tt>(2)});
    BOOST_CHECK(allocation_counter::live() <= live_full);

    copy.clear();
    BOOST_CHECK(allocation_counter::live() < live_full);

    TMap moved(std::move(map));
    BOOST_CHECK(map.empty());
    BOOST_CHECK(!moved.empty());
  }

  // Everything allocated through the allocator has been released.
  BOOST_CHECK_EQUAL(allocation_counter::live(), live_before);
}

BOOST_AUTO_TEST_SUITE_END()
