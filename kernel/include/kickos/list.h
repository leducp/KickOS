// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// A minimal intrusive doubly-linked list. The node (ListNode) embeds in the TCB
// and is SHARED by the ready lists and the wait queues: a thread is on exactly
// one at a time -- ready XOR blocked. That exclusivity is why detach_current()
// must run before a thread parks on a wait queue, else the shared link clobbers.
// The timer delta list (tnext) is a separate, singly-linked concern.

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

    // Doubly-linked, null-terminated, FIFO-insert. No ownership: nodes live in
    // the objects that embed them (TCBs).
    struct List
    {
        ListNode* head = nullptr;
        ListNode* tail = nullptr;

        bool empty() const
        {
            return head == nullptr;
        }

        // KICKOS_DEBUG guards (compiled out by default). Queue integrity rests on
        // caller discipline: a node inserted twice, or unlinked from a list it is not
        // on, corrupts the links silently and surfaces somewhere else.
#if KICKOS_DEBUG
        bool contains(ListNode const* n) const
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
#endif

        void push_back(ListNode* n)
        {
            // A detached node has both links null and is on no list. The link check is
            // O(1); the scan catches a node whose links were nulled while still listed.
            KICKOS_DEBUG_ASSERT(n->next == nullptr and n->prev == nullptr);
            KICKOS_DEBUG_ASSERT(not contains(n));
            KICKOS_DEBUG_ASSERT(n != tail);
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
            // queue bounded by KICKOS_MAX_THREADS.
            KICKOS_DEBUG_ASSERT(contains(n));
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
    };
}

#endif
