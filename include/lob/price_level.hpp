#pragma once

#include <cstdint>

#include "lob/types.hpp"

namespace lob {

// One price level: an intrusive doubly linked FIFO of resting orders plus their total quantity.
struct PriceLevel {
  Order* head = nullptr;
  Order* tail = nullptr;
  uint64_t totalQty = 0;

  void push_back(Order* o) {
    o->prev = tail;
    o->next = nullptr;
    if (tail != nullptr) {
      tail->next = o;
    } else {
      head = o;
    }
    tail = o;
    totalQty += o->remaining;
  }

  // Removes o from the list in O(1) and takes its remaining quantity off the total.
  void unlink(Order* o) {
    if (o->prev != nullptr) {
      o->prev->next = o->next;
    } else {
      head = o->next;
    }
    if (o->next != nullptr) {
      o->next->prev = o->prev;
    } else {
      tail = o->prev;
    }
    totalQty -= o->remaining;
    o->prev = nullptr;
    o->next = nullptr;
  }
};

}  // namespace lob
