// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Kernel address-space management. Borrowed mappings must be unmapped
// before destroying a space, which frees its owned frames.

#ifndef KICKOS_ASPACE_H
#define KICKOS_ASPACE_H

#include <kickos/arch/arch.h>
#include <kickos/budget.h>
#include <kickos/held.h>
#include <kickos/presync.h>
#include <kickos/vrange.h>

#include <stddef.h>
#include <stdint.h>

// The kernel can reach user memory a task holds non-cacheable through a cacheable view of its own.
#define KICKOS_ALIAS_DCACHE (KICKOS_ARCH_ALIAS_DCACHE or KICKOS_ARCH_ARENA_DCACHE)

namespace kickos
{
    struct Thread;
    struct Domain;
    struct Task;
    struct ImageSpan; // kickos/image.h
    class MpuSet;

    // What answers for a user end: its space where the backend translates, else the thread
    // whose region set holds it. Null names kernel storage, and every user end of a region
    // backend with no data cache over the arena.
#if KICKOS_HAVE_ASPACE
    using UserOwner = struct arch_aspace*;
#else
    using UserOwner = Thread const*;
#endif

#if KICKOS_HAVE_ASPACE or KICKOS_ARCH_ARENA_DCACHE
    // The owner of a thread's own user pointers.
    UserOwner user_space_of(Thread const* t);
#else
    inline UserOwner user_space_of(Thread const* t)
    {
        (void)t;
        return nullptr;
    }
#endif

    // Whether the kernel may store into [base, base + n) of user memory under the map this core
    // has loaded now; every store a call makes asks first, and answers -KOS_EFAULT where not.
    inline bool kstore_admitted(uintptr_t base, size_t n)
    {
#if ARCH_USER_STORE_CHECKED
        return arch_mpu_loaded_writable(base, n);
#else
        (void)base;
        (void)n;
        return true;
#endif
    }

    // Copy through each owning space's acquire interface, one granule at a time.
    // Failure can leave a copied prefix. Return an error rather than panic;
    // the fault reporter also uses these functions.
    [[nodiscard]] bool kaccess_from_user(void* kdst, UserOwner sspace, uintptr_t usrc, size_t n);
    [[nodiscard]] bool kaccess_to_user(UserOwner dspace, uintptr_t udst, void const* ksrc,
                                       size_t n);

    // Copy one pointer-sized word through one acquire; kword MUST be pointer-aligned.
    // Return false for a misaligned udst or failed acquisition.
    [[nodiscard]] bool kaccess_word_to_user(UserOwner dspace, uintptr_t udst, void const* kword);

    // Copy between user ranges, each with its own space.
    // Reject overlapping ranges in the same space.
    [[nodiscard]] bool ep_copy(UserOwner dspace, uintptr_t dst, UserOwner sspace, uintptr_t src,
                               size_t n);

#if KICKOS_ALIAS_DCACHE
    // Maintain the data cache over a kernel pointer that reaches non-cacheable user memory
    // cacheably: before the kernel reads or writes through it, and again after it writes.
    void alias_sync(void const* p, size_t n);
#endif

#if KICKOS_ARCH_ARENA_DCACHE and not KICKOS_HAVE_ASPACE
    // Ahead of a memory region of `attr` over [base, base + size): one that is non-cacheable,
    // one that retypes a region `held` names at exactly that extent, or a cacheable one over a
    // block a non-cacheable region held since its last sync (ram_owner_sync_owed), has the
    // block's lines cleaned to memory then dropped, so none an earlier use left is evicted over
    // it or read through it. The block then owes the next cacheable region a sync exactly when
    // this one is non-cacheable. Caller holds IrqLock.
    void grant_sync(MpuSet const* held, uintptr_t base, size_t size, uint32_t attr);
#else
    inline void grant_sync(MpuSet const* held, uintptr_t base, size_t size, uint32_t attr)
    {
        (void)held;
        (void)base;
        (void)size;
        (void)attr;
    }
#endif

#if KICKOS_HAVE_ASPACE

    // A task reaches a frame the kernel hands it at the frame's physical address plus the
    // arch's user offset (arch_aspace_user_offset): one rule on every translating backend.
    inline uintptr_t aspace_user_va(arch_phys_addr_t frame)
    {
        return static_cast<uintptr_t>(frame) + arch_aspace_user_offset();
    }
    inline arch_phys_addr_t aspace_frame_of(uintptr_t va)
    {
        return static_cast<arch_phys_addr_t>(va - arch_aspace_user_offset());
    }

    // Seed image `image`'s shared RX text, its private RW data and matching validation ranges.
    // The data is a copy of the frames the loader put the image's window in, which no space maps;
    // no space's live data is ever a source. 0, -KOS_EAGAIN past `payer`'s PAGES budget, or
    // -KOS_ENOMEM; on failure, release the partially built space with aspace_release.
    int aspace_image_seed(Task const* payer, struct arch_aspace* space, VirtualRanges* ranges,
                          size_t image);

    // Whether `payer` may take the table frames a map of [va, va + pages) adds to `space`. Every
    // map site asks it before arch_aspace_map. Caller holds IrqLock.
    bool aspace_tables_admit(Task const* payer, struct arch_aspace* space, uintptr_t va,
                             size_t pages);

    // What `d`'s space holds of `kind`: RAM_BLOCKS or RAM_BYTES of the reservations kos_ram_alloc
    // made in it, PAGES of the frames it owns (its data copy, stacks and reservations) and of its
    // tables. A space dies with its task's incarnation, so this is the incarnation's. O(the
    // space's ranges, plus its tables' entries for PAGES). Null-safe: 0. Caller holds IrqLock.
    uint32_t aspace_task_usage(Domain const* d, BudgetKind kind);

    // Image `image`'s kickos_task_start where each space of it reaches it: the entry
    // thread_create gives every unprivileged thread of that image.
    void (*aspace_task_start(size_t image))(void*);

    // The images the link states the frames of, which is every image the link holds.
    size_t aspace_image_frames_count();

    // Unmap what the space borrows, return the frames of a reservation it never mapped, then
    // destroy it. The one sanctioned way to end a space; arch_aspace_destroy alone strands both.
    // `keep_lent` frees no frame of a lent reservation, which the space must not map.
    void aspace_release(struct arch_aspace* space, VirtualRanges* ranges, bool keep_lent = false);

    // Whether `e` is a reservation another space's handoff or memory window maps.
    bool aspace_range_lent(VirtualRange const* e);
    // At its task's death: end the space and everything in it but its lent reservations, which
    // `ranges` alone goes on naming. Caller holds IrqLock.
    void aspace_shed(struct arch_aspace* space, VirtualRanges* ranges);
    // The pages a shed space's ranges still name.
    uint32_t aspace_shed_pages(VirtualRanges const* ranges);
    // Free the frames of the reservation in slot `i` a shed space names, and forget it.
    void aspace_shed_free(VirtualRanges* ranges, size_t i);

    // Unmap every frame run the space maps and drop each mapping's reference. Caller holds
    // IrqLock.
    void aspace_release_runs(struct arch_aspace* space, VirtualRanges* ranges);

    // Whether every live mapping of [pa, pa + pages granules) in any space, `self` aside, carries
    // `memtype`: one block mapped cacheable in one place and not in another is incoherent.
    bool aspace_frames_type_ok(arch_phys_addr_t pa, size_t pages, uint8_t memtype,
                               VirtualRange const* self);

    // Whether `space`, whose list is `ranges`, maps the frame holding `pa` through a granted
    // range with write. Stacks and the image are not asked: a stack's frames are mapped in its
    // own space alone, and every space holds its own copy of the image data.
    bool aspace_maps_writable(struct arch_aspace* space, VirtualRanges const* ranges,
                              arch_phys_addr_t pa);

    // Whether mapping granted range `e` again with `rights` and `memtype` takes from its space
    // a write to normal memory that the range gave.
    inline bool aspace_remap_takes_write(VirtualRange const& e, uint32_t rights, uint8_t memtype)
    {
        uint8_t const normal = static_cast<uint8_t>(ARCH_MAP_NORMAL);
        bool const gave = e.state == VirtualState::Granted and (e.rights & ARCH_MAP_W) != 0
                          and e.memtype == normal;
        return gave and ((rights & ARCH_MAP_W) == 0 or memtype != normal);
    }

    // Whether mapping `e`'s frames with `memtype` owes the kernel's cacheable view of them a
    // sync: a non-cacheable mapping, or any change of type over a granted range. A cacheable
    // mapping over frames a non-cacheable one held since their last sync owes one too
    // (VR_SYNC_OWED, kernel/mem/aspace.cc).
    inline bool aspace_grant_syncs(VirtualRange const* e, uint8_t memtype)
    {
        if (memtype == static_cast<uint8_t>(ARCH_MAP_NOCACHE))
        {
            return true;
        }
        return e != nullptr and e->state == VirtualState::Granted and e->memtype != memtype;
    }

#if KICKOS_PRESYNC
    // The work a system call does outside the kernel lock (kickos/presync.h). All but
    // presync_run and presync_release are called under IrqLock.
    //
    // The calling thread's record, or null.
    PresyncRecord* presync_record();
    // Start a round: forget what the last one noted and mapped. True on the call's first round,
    // whose parameters the caller then copies into the record.
    bool presync_begin();
#if KICKOS_ARCH_ALIAS_DCACHE
    // Note a span the locked pass will owe a sync. Frames outside the pool are not noted.
    void presync_note(arch_phys_addr_t pa, size_t pages);
#endif
    // Stage the copy of declaration `decl`'s image's pristine data that aspace_image_seed will
    // map into its next space, charged to the declaration's record, which no other create may
    // seat while it is staged. Past the record's PAGES budget, or while another create holds
    // a stage for it, nothing is staged and the locked pass refuses.
    void presync_stage_image(size_t decl);
    // Whether a thread other than `asker`, any thread where it is null, holds an image stage for
    // `t`'s record.
    bool presync_image_staged_elsewhere(Task const* t, Thread const* asker);
    // No lock held: sync every noted frame, then fill the staged run.
    void presync_run();
    // The call completed: every other live record meeting what its locked pass mapped is dropped.
    void presync_commit();
    // End a round: true when the locked pass was refused and the call goes round again.
    bool presync_end();
    // No lock held: free a staged run the round did not take, which a call that `succeeded`
    // and was not sent round cannot have. Pokes the INSTANCE watches (start_watch_poke) where
    // this drops a declaration's stager mark, or where the call took one and did not succeed.
    void presync_release(bool succeeded);
    // The calling thread is leaving inside a call: drop its record and free its stage.
    void presync_exit();
    // One locked pass taking back the frame map the calling thread holds in flight, ending the
    // waits on the words it unmapped: false once none is left.
    bool presync_map_unwind_pass(Held held);
    // The completed frame map the calling thread still holds in flight becomes one its
    // presync_map_unwind_pass takes back: false, forgetting it, when the range is no longer
    // the one it mapped. Caller holds IrqLock.
    bool presync_map_retract();
    // One pass of at most ASPACE_TABLE_PASS_FRAMES table frames toward `n` for the calling
    // thread's later passes, charged to `payer`'s PAGES, so no later pass of its call can be
    // refused a table: 0 once it holds `n`, 1 while short, -KOS_EAGAIN where `payer` cannot take
    // them, -KOS_ENOMEM where the pool cannot. What it took stays held, for the release passes.
    // Caller holds IrqLock.
    int presync_tables_take_pass(Task const* payer, size_t n);
    // The reserve hands its frames out from now on, `n` of them charged to `payer`: 0, or the
    // refusal of a reserve short of `n`. Leftovers stay held for the release passes. O(1).
    int presync_tables_arm(Task const* payer, size_t n);
    // Whether the calling thread holds an armed reserve.
    bool presync_tables_held();
    // One pass giving back at most ASPACE_TABLE_PASS_FRAMES of the calling thread's reserved
    // table frames: false once none is left. Caller holds IrqLock.
    bool presync_tables_release_pass();
}

extern "C"
{
    // One of the calling thread's reserved table frames, or 0: kickos_frame_alloc's first
    // source. Caller holds IrqLock.
    arch_phys_addr_t kickos_presync_table_take(void);
}

namespace kickos
{
    // `t` is a slot's new occupant: its record is a new one.
    void presync_fresh(Thread const* t);
    // What `t`'s members hold staged, which no range names yet, of `kind`: the PAGES of a
    // reservation or a frame run, and the RAM_BLOCKS and RAM_BYTES of a reservation. A new
    // space's staged data copy is the created task's, never its creator's. O(t's members).
    // Caller holds IrqLock.
    uint32_t presync_task_staged(Task const* t, BudgetKind kind);

    // The frames of `bytes`, staged for presync_run to clear: a RESERVE for kos_ram_alloc, a
    // CLEAR for a frame run. 0 when the pool has no run that long.
    arch_phys_addr_t aspace_reserve_stage(VirtualRanges const* ranges, size_t bytes,
                                          PresyncStage stage);
    // Reserve the cleared RESERVE run in `ranges`: its address, or 0 with the run left staged.
    uintptr_t aspace_reserve_commit(VirtualRanges* ranges);
    // Hand the cleared CLEAR run to the empty frame run `run_obj`: false, with the run left
    // staged, unless every granule of it was cleared.
    bool aspace_reserve_run(int run_obj);

#if KICKOS_ARCH_ALIAS_DCACHE
    // The spans the locked pass of each mapping call below will owe a sync, noted ahead of it.
    // Each asks what that call asks and notes nothing where it would refuse.
    void aspace_self_grant_note(VirtualRanges const* ranges, uintptr_t base, size_t size,
                                uint32_t rights, enum arch_map_memtype type);
    void aspace_cap_map_note(int run_obj, arch_phys_addr_t base, uint32_t pages,
                             enum arch_map_memtype type);
    // A task's declared block or data: a whole reservation of `own`'s at `base`.
    void aspace_reservation_note(VirtualRanges const* own, uintptr_t base, size_t bytes,
                                 enum arch_map_memtype type);
#endif
#endif

    // The most pages one locked pass maps or unmaps of a run whose size a caller chose.
    constexpr size_t ASPACE_MAP_PASS_PAGES = 32u;
    // The most table frames one locked pass takes into, or gives back from, a reserve.
    constexpr size_t ASPACE_TABLE_PASS_FRAMES = 32u;

    // What one locked pass mapped or unmapped, and whether the edit is complete.
    struct MapPass
    {
        uintptr_t base = 0;
        size_t bytes = 0;
        bool done = false;
    };

    // One locked pass of the self-grant: map at most ASPACE_MAP_PASS_PAGES more pages of the
    // range the caller reserved, at the address it reserved. Until a pass maps its last page the
    // range is pending (VirtualRange::pending): it keeps its state and type for every other
    // reader, another mapping of its frames with another type is refused -KOS_EBUSY, a
    // self-grant of it with another type is refused -KOS_EBUSY, one with its type resumes it,
    // and a teardown frees each of its frames once. A refused pass maps nothing and leaves it
    // pending.
    // -KOS_EPERM for an address this space never reserved, a cross-task self-grant included;
    // -KOS_EBUSY when another mapping of its frames carries another memory type; -KOS_EAGAIN
    // when `payer` cannot take the tables; 0 with `done` when the range is already mapped with
    // these attributes.
    int aspace_self_grant(Task const* payer, struct arch_aspace* space, VirtualRanges* ranges,
                          uintptr_t base, size_t size, uint32_t rights,
                          enum arch_map_memtype type, MapPass* pass);

    // One locked pass of a teardown that every member of the space's task is past: unmap, and
    // free where the space owns them, at most ASPACE_MAP_PASS_PAGES pages of its frame runs,
    // handoff and reservations, leaving the destroy walk the image and the stacks. False once
    // none is left.
    bool aspace_trim_pass(struct arch_aspace* space, VirtualRanges* ranges);

    // Image `image`'s spans as its tasks reach them: its read-only image, its writable one, and
    // its thread-local template.
    void aspace_image_spans(size_t image, ImageSpan* code, ImageSpan* ram, ImageSpan* tls);

    // Return the kernel alias of one byte of image `image`, even before spaces exist.
    // Return null outside that image.
    void* aspace_image_alias(size_t image, void const* app_ptr);

    // One locked pass of a capability-owned frame run's map at *at, or at the run's own address
    // where *at is 0, recorded as borrowed: at most ASPACE_MAP_PASS_PAGES more of its pages. On
    // 0 with `done`, *at is where it was mapped. The capability retains frame ownership. Until a
    // pass maps its last page the mapping is pending, as a self-grant's range is: a map of the
    // same run at the same address and type resumes it, and with another type is refused
    // -KOS_EBUSY. A mapping it starts is `holder`'s (a thread slot index plus one), and a
    // refused first pass leaves nothing; a later one leaves it pending for its holder to take
    // back (aspace_cap_map_unwind). Where `resuming`, a pass of a map whose earlier passes
    // mapped something, the same mapping found complete is done. Return -KOS_ENOMEM for
    // unavailable space, -KOS_EINVAL for an invalid range, -KOS_EBUSY where another mapping of
    // the run carries another memory type, or -KOS_EAGAIN where `payer`, the space's, cannot
    // take the tables.
    int aspace_cap_map(Task const* payer, struct arch_aspace* space, VirtualRanges* ranges,
                       uintptr_t* at, int run_obj, arch_phys_addr_t base, uint32_t pages,
                       uint32_t rights, enum arch_map_memtype type, MapPass* pass,
                       uint16_t holder = 0, bool resuming = false);

    // One locked pass taking back the pending map `holder` started at `va`: at most
    // ASPACE_MAP_PASS_PAGES of its mapped prefix unmapped from the top, and at the last the
    // range released with its reference. `done` once nothing of it is left, at once where no
    // such map is there.
    void aspace_cap_map_unwind(struct arch_aspace* space, VirtualRanges* ranges, uintptr_t va,
                               uint16_t holder, MapPass* pass);

    // One locked pass of a capability mapping's unmap: at most ASPACE_MAP_PASS_PAGES of its top
    // pages, the mapping left that much shorter; the last pass releases its reference. Return
    // -KOS_EPERM unless both the mapping kind and run_obj match (VR_BORROWED alone is
    // insufficient), or -KOS_EBUSY while its map is pending.
    int aspace_cap_unmap(struct arch_aspace* space, VirtualRanges* ranges, uintptr_t va,
                         int run_obj, MapPass* pass);

    // Map a spawn window's `bytes` at `pa` where the backend's window area has room, recorded as
    // a VR_WINDOW range held by `holder` (a thread slot index plus one) at `place` in its spawn
    // list. A memory window names the `donor` domain whose reservation it maps and holds a
    // reference on it until unmapped; a device window names none. Maps what `budget` has left,
    // one page at least, and takes it off `budget`; the rest stays pending for
    // aspace_create_pass. -KOS_ENOMEM when the area, the list or the tables are full,
    // -KOS_EAGAIN when `payer` cannot take the tables.
    int aspace_window_map(Task const* payer, struct arch_aspace* space, VirtualRanges* ranges,
                          arch_phys_addr_t pa, size_t bytes, uint32_t rights,
                          enum arch_map_memtype type, uint16_t holder, uint32_t place,
                          Domain* donor, size_t* budget);

    // Whether a handoff of `ranges`, or a window `holder` holds there, is pending.
    bool aspace_create_pending(VirtualRanges const* ranges, uint16_t holder);

    // The table frames the rest of the create's pending maps for `holder` can need.
    size_t aspace_create_tables(VirtualRanges const* ranges, uint16_t holder);

    // One locked pass of a create's maps: at most ASPACE_MAP_PASS_PAGES more pages of a pending
    // handoff of the space, or of a pending window `holder` holds there. 1 when it mapped some,
    // 0 when none is pending, -KOS_EAGAIN when `payer` cannot take the tables, -KOS_ENOMEM.
    int aspace_create_pass(Task const* payer, struct arch_aspace* space, VirtualRanges* ranges,
                           uint16_t holder, MapPass* pass);

    // One locked pass of the unmap of the windows `holder` holds: at most ASPACE_MAP_PASS_PAGES
    // off the top of one, completing the backend's shootdown, the window left that much shorter
    // and released at its last page. A memory window released names in *lender the domain whose
    // reservation at *lent it mapped, which the caller then unlends and releases; null for
    // none. False once `holder` holds none.
    bool aspace_window_unmap_pass(struct arch_aspace* space, VirtualRanges* ranges,
                                  uint16_t holder, MapPass* pass, Domain** lender,
                                  uintptr_t* lent);

    // Whether `holder` holds a window in `ranges`. Null-safe: false.
    bool aspace_window_held(VirtualRanges const* ranges, uint16_t holder);

    // Every pass of aspace_window_unmap_pass in one go. A holder with none is a no-op.
    void aspace_window_unmap_holder(struct arch_aspace* space, VirtualRanges* ranges,
                                    uint16_t holder);

    // aspace_handoff's refusal that its arguments alone decide: 0, or -KOS_EPERM where
    // [base, base + size) is not one whole reservation `donor` may name.
    int aspace_handoff_admit(VirtualRanges const* donor, uintptr_t base, size_t size);

    // Map the donor's complete reservation at the same VA without taking ownership, one pass of
    // it, the rest pending for aspace_create_pass. Require its exact base and rounded page
    // count. Return -KOS_EPERM for a missing reservation, -KOS_EBUSY where the frames are mapped
    // with another memory type, -KOS_EAGAIN where `payer` cannot take the tables, or
    // -KOS_ENOMEM if the destination cannot accept it.
    int aspace_handoff(Task const* payer, VirtualRanges const* donor, struct arch_aspace* space,
                       VirtualRanges* ranges, uintptr_t base, size_t size,
                       enum arch_map_memtype type);

    // Whether `e` is a handoff: frames another domain reserved.
    bool aspace_range_handed_off(VirtualRange const* e);

    // Unmap and forget the handoff `ranges` holds, freeing no frame.
    void aspace_release_handoff(struct arch_aspace* space, VirtualRanges* ranges);

    // Install and return the thread's task space, skipping redundant root writes.
    // For a spaceless thread, and above one kernel core for one whose exit has left its task's
    // space (Thread::left_space), return null; SMP installs the boot root, while single-core
    // builds retain the current root.
    struct arch_aspace* aspace_activate_for(Thread const* t);

    // Install the boot root on THIS core and record it. For a caller about to stop being a
    // member of the space it is running in: the last member's release frees these tables.
    void aspace_install_boot(void);

#else

    inline struct arch_aspace* aspace_activate_for(Thread const*) { return nullptr; }
    inline void aspace_install_boot(void) {}
    inline void* aspace_image_alias(size_t, void const*) { return nullptr; }

#endif
}

#endif
