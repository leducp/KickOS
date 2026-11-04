// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// A minimal intrusive doubly-linked list. The TCB's `link` node is SHARED by the ready
// lists, the wait queues, and a server's reply-donor list (HeadList below), so a thread may
// be on exactly one of the three at a time: ready XOR blocked-on-a-waitq XOR
// waiting-for-a-reply. detach_current(), or a wq_pop_highest, MUST run before the thread
// is pushed onto the next list, else the shared links are clobbered.

#ifndef KICKOS_LIST_H
#define KICKOS_LIST_H

#include <stddef.h>

#include <kickos/debug.h> // KICKOS_DEBUG_ASSERT (a leaf; kernel.h would close a cycle)

// Recover the enclosing struct from a pointer to one of its members.
#define KICKOS_CONTAINER_OF(ptr, type, member) \
    reinterpret_cast<type*>(reinterpret_cast<char*>(ptr) - offsetof(type, member))

namespace kickos
{
    struct ListNode
    {
        ListNode* next = nullptr;
        ListNode* prev = nullptr;
    };

    inline bool list_contains(ListNode const* head, ListNode const* n)
    {
        for (ListNode const* c = head; c != nullptr; c = c->next)
        {
            if (c == n)
            {
                return true;
            }
        }
        return false;
    }

    // FIFO-insert. No ownership: nodes live in the objects that embed them (TCBs).
    struct List
    {
        ListNode* head = nullptr;
        ListNode* tail = nullptr;

        bool empty() const
        {
            return head == nullptr;
        }

        // Whether `n` is on this list, for a node on this list or on none: a node on another
        // list answers true. O(1).
        bool holds(ListNode const* n) const
        {
            return n->prev != nullptr or head == n;
        }

        // The membership asserts are compiled out by default, so queue integrity rests on
        // caller discipline: a node inserted twice, or unlinked from a list it is not on,
        // corrupts the links silently and surfaces somewhere else entirely.
        void push_back(ListNode* n)
        {
            // A detached node has both links null. The scan additionally catches a node
            // whose links were nulled while it was still listed.
            KICKOS_DEBUG_ASSERT(n->next == nullptr and n->prev == nullptr);
            KICKOS_DEBUG_ASSERT(not list_contains(head, n));
            n->next = nullptr;
            n->prev = tail;
            if (tail != nullptr)
            {
                tail->next = n;
            }
            else
            {
                head = n;
            }
            tail = n;
        }

        void unlink(ListNode* n)
        {
            // Unlinking a node not on this list splices this list's head/tail onto that
            // node's neighbours, corrupting both lists quietly. The scan is O(n) over a
            // queue bounded by the thread pool.
            KICKOS_DEBUG_ASSERT(list_contains(head, n));
            if (n->prev != nullptr)
            {
                n->prev->next = n->next;
            }
            else
            {
                head = n->next;
            }
            if (n->next != nullptr)
            {
                n->next->prev = n->prev;
            }
            else
            {
                tail = n->prev;
            }
            n->next = nullptr;
            n->prev = nullptr;
        }

        // Unlink and return the first node, nullptr when empty. O(1).
        ListNode* pop_front()
        {
            ListNode* const n = head;
            if (n == nullptr)
            {
                return nullptr;
            }
            head = n->next;
            if (head != nullptr)
            {
                head->prev = nullptr;
            }
            else
            {
                tail = nullptr;
            }
            n->next = nullptr;
            return n;
        }
    };

    // The same ListNode, one head word instead of two: no tail, so insertion is LIFO and
    // there is no back(). Removal stays O(1). Use List wherever order or the tail matters.
    struct HeadList
    {
        ListNode* head = nullptr;

        bool empty() const
        {
            return head == nullptr;
        }

        // As List::holds: for a node on this list or on none. O(1).
        bool holds(ListNode const* n) const
        {
            return n->prev != nullptr or head == n;
        }

        void push(ListNode* n)
        {
            KICKOS_DEBUG_ASSERT(n->next == nullptr and n->prev == nullptr);
            KICKOS_DEBUG_ASSERT(not list_contains(head, n));
            n->prev = nullptr;
            n->next = head;
            if (head != nullptr)
            {
                head->prev = n;
            }
            head = n;
        }

        // Unlink and return the node pushed last, nullptr when empty. O(1).
        ListNode* pop()
        {
            ListNode* const n = head;
            if (n == nullptr)
            {
                return nullptr;
            }
            head = n->next;
            if (head != nullptr)
            {
                head->prev = nullptr;
            }
            n->next = nullptr;
            return n;
        }

        // `n` must be on this list. O(1).
        void unlink(ListNode* n)
        {
            KICKOS_DEBUG_ASSERT(list_contains(head, n));
            detach(n);
        }

        // False means `n` was not on this list, which is NOT an error here; see
        // reply_donor_unpark. O(members), bounded by one server's outstanding callers.
        bool unlink_if_present(ListNode* n)
        {
            if (not list_contains(head, n))
            {
                return false;
            }
            detach(n);
            return true;
        }

    private:
        void detach(ListNode* n)
        {
            if (n->prev != nullptr)
            {
                n->prev->next = n->next;
            }
            else
            {
                head = n->next;
            }
            if (n->next != nullptr)
            {
                n->next->prev = n->prev;
            }
            n->next = nullptr;
            n->prev = nullptr;
        }
    };
}

#endif
