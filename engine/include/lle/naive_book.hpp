#pragma once
// Baseline book used only for before/after measurements: the textbook design with
// std::map price levels and std::unordered_map orders. Same interface as MarketBook.
// Every new price level and every new order allocates.
#include <cstdint>
#include <functional>
#include <map>
#include <unordered_map>
#include <vector>

#include "lle/book.hpp"
#include "lle/common.hpp"

namespace lle {

class NaiveBook {
 public:
  explicit NaiveBook(const BookConfig& cfg = {}) : cfg_(cfg) {}

  int add_instrument(std::uint16_t locate) {
    books_.emplace_back();
    loc_[locate] = static_cast<int>(books_.size() - 1);
    return static_cast<int>(books_.size() - 1);
  }
  int book_for(std::uint16_t locate) const {
    auto it = loc_.find(locate);
    return it == loc_.end() ? -1 : it->second;
  }
  std::size_t num_books() const { return books_.size(); }

  BookEvent add(std::uint64_t ref, std::uint16_t locate, Side side, std::uint32_t raw_price, std::uint32_t qty) {
    const int b = book_for(locate);
    if (b < 0) return {};
    const std::int64_t px = raw_price / cfg_.price_scale;
    auto& bk = books_[b];
    bool top;
    if (side == Side::Buy) {
      top = bk.bids.empty() || px >= bk.bids.begin()->first;
      bk.bids[px] += qty;
    } else {
      top = bk.asks.empty() || px <= bk.asks.begin()->first;
      bk.asks[px] += qty;
    }
    orders_[ref] = Order{px, qty, b, side};
    return {static_cast<std::int16_t>(b), top};
  }

  BookEvent reduce(std::uint64_t ref, std::uint32_t qty, std::int64_t* exec_price = nullptr,
                   Side* exec_side = nullptr) {
    auto it = orders_.find(ref);
    if (it == orders_.end()) return {};
    Order& o = it->second;
    const std::uint32_t take = qty < o.qty ? qty : o.qty;
    if (exec_price) *exec_price = o.price;
    if (exec_side) *exec_side = o.side;
    const bool top = take_from_level(o, take);
    o.qty -= take;
    const int b = o.book;
    if (o.qty == 0) orders_.erase(it);
    return {static_cast<std::int16_t>(b), top};
  }

  BookEvent remove(std::uint64_t ref) {
    auto it = orders_.find(ref);
    if (it == orders_.end()) return {};
    const bool top = take_from_level(it->second, it->second.qty);
    const int b = it->second.book;
    orders_.erase(it);
    return {static_cast<std::int16_t>(b), top};
  }

  BookEvent replace(std::uint64_t old_ref, std::uint64_t new_ref, std::uint32_t qty, std::uint32_t raw_price) {
    auto it = orders_.find(old_ref);
    if (it == orders_.end()) return {};
    Order o = it->second;
    const bool top1 = take_from_level(o, o.qty);
    orders_.erase(it);
    const std::int64_t px = raw_price / cfg_.price_scale;
    auto& bk = books_[o.book];
    bool top2;
    if (o.side == Side::Buy) {
      top2 = bk.bids.empty() || px >= bk.bids.begin()->first;
      bk.bids[px] += qty;
    } else {
      top2 = bk.asks.empty() || px <= bk.asks.begin()->first;
      bk.asks[px] += qty;
    }
    orders_[new_ref] = Order{px, qty, o.book, o.side};
    return {static_cast<std::int16_t>(o.book), top1 || top2};
  }

  std::int64_t best_bid(int b) const { return books_[b].bids.empty() ? INT64_MIN : books_[b].bids.begin()->first; }
  std::int64_t best_ask(int b) const { return books_[b].asks.empty() ? INT64_MIN : books_[b].asks.begin()->first; }
  std::int64_t best_bid_qty(int b) const { return books_[b].bids.empty() ? 0 : books_[b].bids.begin()->second; }
  std::int64_t best_ask_qty(int b) const { return books_[b].asks.empty() ? 0 : books_[b].asks.begin()->second; }

  std::uint64_t digest() const {
    std::uint64_t h = 0;
    for (const auto& b : books_) {
      std::uint64_t hb = 1469598103934665603ull, ha = 1469598103934665603ull;
      for (auto& [p, q] : b.bids) {
        hb = (hb ^ static_cast<std::uint64_t>(p)) * 1099511628211ull;
        hb = (hb ^ static_cast<std::uint64_t>(q)) * 1099511628211ull;
      }
      for (auto& [p, q] : b.asks) {
        ha = (ha ^ static_cast<std::uint64_t>(p)) * 1099511628211ull;
        ha = (ha ^ static_cast<std::uint64_t>(q)) * 1099511628211ull;
      }
      h = h * 31 + (hb ^ (ha << 1));
    }
    return h;
  }

 private:
  struct Order {
    std::int64_t price;
    std::uint32_t qty;
    int book;
    Side side;
  };
  struct Book {
    std::map<std::int64_t, std::int64_t, std::greater<>> bids;
    std::map<std::int64_t, std::int64_t> asks;
  };
  bool take_from_level(const Order& o, std::uint32_t qty) {
    auto& bk = books_[o.book];
    if (o.side == Side::Buy) {
      auto it = bk.bids.find(o.price);
      if (it == bk.bids.end()) return false;
      const bool top = it == bk.bids.begin();
      if ((it->second -= qty) <= 0) bk.bids.erase(it);
      return top;
    }
    auto it = bk.asks.find(o.price);
    if (it == bk.asks.end()) return false;
    const bool top = it == bk.asks.begin();
    if ((it->second -= qty) <= 0) bk.asks.erase(it);
    return top;
  }

  BookConfig cfg_;
  std::vector<Book> books_;
  std::unordered_map<std::uint16_t, int> loc_;
  std::unordered_map<std::uint64_t, Order> orders_;
};

}  // namespace lle
