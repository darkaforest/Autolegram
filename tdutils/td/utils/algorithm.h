//
// Copyright Aliaksei Levin (levlam@telegram.org), Arseny Smirnov (arseny30@gmail.com) 2014-2022
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
#pragma once

#include "td/utils/common.h"

#include <functional>
#include <type_traits>
#include <utility>

namespace td {

namespace detail {

template <typename V>
struct transform_helper {
  template <class Func>
  auto transform(const V &v, const Func &f) {
    vector<decltype(f(*v.begin()))> result;
    result.reserve(v.size());
    for (auto &x : v) {
      result.push_back(f(x));
    }
    return result;
  }

  template <class Func>
  auto transform(V &&v, const Func &f) {
    vector<decltype(f(std::move(*v.begin())))> result;
    result.reserve(v.size());
    for (auto &x : v) {
      result.push_back(f(std::move(x)));
    }
    return result;
  }
};

}  // namespace detail

template <class V, class Func>
auto transform(V &&v, const Func &f) {
  return detail::transform_helper<std::decay_t<V>>().transform(std::forward<V>(v), f);
}

template <class V, class Func>
bool remove_if(V &v, const Func &f) {
  size_t i = 0;
  while (i != v.size() && !f(v[i])) {
    i++;
  }
  if (i == v.size()) {
    return false;
  }

  size_t j = i;
  while (++i != v.size()) {
    if (!f(v[i])) {
      v[j++] = std::move(v[i]);
    }
  }
  v.erase(v.begin() + j, v.end());
  return true;
}

template <class V, class T>
bool remove(V &v, const T &value) {
  size_t i = 0;
  while (i != v.size() && v[i] != value) {
    i++;
  }
  if (i == v.size()) {
    return false;
  }

  size_t j = i;
  while (++i != v.size()) {
    if (v[i] != value) {
      v[j++] = std::move(v[i]);
    }
  }
  v.erase(v.begin() + j, v.end());
  return true;
}

template <class V>
void unique(V &v) {
  if (v.empty()) {
    return;
  }

  // use ADL to find std::sort
  // caller will need to #include <algorithm>
  sort(v.begin(), v.end(), std::less<void>());

  size_t j = 1;
  for (size_t i = 1; i < v.size(); i++) {
    if (v[i] != v[j - 1]) {
      if (i != j) {
        v[j] = std::move(v[i]);
      }
      j++;
    }
  }
  v.resize(j);
}

template <class V, class T>
bool contains(const V &v, const T &value) {
  for (auto &x : v) {
    if (x == value) {
      return true;
    }
  }
  return false;
}

template <class V, class F>
bool all_of(const V &v, F &&f) {
  for (const auto &x : v) {
    if (!f(x)) {
      return false;
    }
  }
  return true;
}

template <class T>
void reset_to_empty(T &value) {
  using std::swap;
  std::decay_t<T> tmp;
  swap(tmp, value);
}

template <class T>
void append(vector<T> &destination, const vector<T> &source) {
  destination.insert(destination.end(), source.begin(), source.end());
}

template <class T>
void append(vector<T> &destination, vector<T> &&source) {
  if (destination.empty()) {
    destination.swap(source);
    return;
  }
  destination.reserve(destination.size() + source.size());
  for (auto &elem : source) {
    destination.push_back(std::move(elem));
  }
  reset_to_empty(source);
}

template <class T>
void combine(vector<T> &destination, const vector<T> &source) {
  append(destination, source);
}

template <class T>
void combine(vector<T> &destination, vector<T> &&source) {
  if (destination.size() < source.size()) {
    destination.swap(source);
  }
  if (source.empty()) {
    return;
  }
  destination.reserve(destination.size() + source.size());
  for (auto &elem : source) {
    destination.push_back(std::move(elem));
  }
  reset_to_empty(source);
}

namespace detail {
template <typename T>
struct reversion_wrapper {
  T &iterable;
};

template <typename T>
auto begin(reversion_wrapper<T> w) {
  return w.iterable.rbegin();
}

template <typename T>
auto end(reversion_wrapper<T> w) {
  return w.iterable.rend();
}
}  // namespace detail

template <typename T>
detail::reversion_wrapper<T> reversed(T &iterable) {
  return {iterable};
}

template <class TableT, class FuncT>
void table_remove_if(TableT &table, FuncT &&func) {
  for (auto it = table.begin(); it != table.end();) {
    if (func(*it)) {
      it = table.erase(it);
    } else {
      ++it;
    }
  }
}

template <class NodeT, class HashT, class EqT>
class FlatHashTable;

template <class NodeT, class HashT, class EqT, class FuncT>
void table_remove_if(FlatHashTable<NodeT, HashT, EqT> &table, FuncT &&func) {
  table.remove_if(func);
}

}  // namespace td
