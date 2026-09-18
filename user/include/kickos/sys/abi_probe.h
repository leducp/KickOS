// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Probe selectors and result encodings. Values are ABI: append, never reorder.
// Include this header explicitly; sys.h and sys/abi.h must not include it.
// Keep the extern "C" block so check_c_headers.sh includes this header.

#ifndef KICKOS_SYS_ABI_PROBE_H
#define KICKOS_SYS_ABI_PROBE_H

#ifdef __cplusplus
extern "C"
{
#endif

// `op` selector for KOS_SYS_DOORBELL_PROBE.
enum kos_doorbell_op
{
    // (core) -> bits 63:32: instruction rendezvous initiated; bits 31:0: all
    // doorbell services, including wakes. Each service synchronizes context.
    // Return zero for cores outside the built range.
    KOS_DOORBELL_OP_COUNTS = 0,
    // () -> machine core count used by the rendezvous matrix, not image core count.
    KOS_DOORBELL_OP_WIDTH = 1,
    // () -> the calling core's matrix row; userspace cannot derive it.
    KOS_DOORBELL_OP_SELF = 2,
    // (from) -> first kernel-reserved IRQ at or above from, or -1 if none.
    // Interpret the unsigned result as signed before checking it.
    KOS_DOORBELL_OP_KERNEL_LINE = 3
};

// `op` selector for KOS_SYS_SCHED_PROBE.
enum kos_sched_op
{
    // Core at the time of the read. Pin the caller to one core for a stable result.
    KOS_SCHED_OP_CORE = 0,
    KOS_SCHED_OP_AFFINITY = 1,   // () -> the caller's own core mask
    KOS_SCHED_OP_TASK_CORES = 2, // () -> the caller's task's core set
    KOS_SCHED_OP_CEILING = 3,    // () -> the caller's task's priority ceiling
    KOS_SCHED_OP_ISOLATED = 4,   // () -> the cores this image isolates
    // () -> monotonic mask of cores that preempted on their own slice timer.
    // Cross-core reschedules and device wakes do not set bits.
    KOS_SCHED_OP_PREEMPTED = 5
};

// `op` selector for KOS_SYS_ASPACE_PROBE.
enum kos_aspace_op
{
    KOS_ASPACE_OP_GRANULE = 0,   // () -> the map editor's granule in bytes
    KOS_ASPACE_OP_MEMTYPE = 1,   // (enum arch_map_memtype as a number) -> 1 honoured, 0 not
    KOS_ASPACE_OP_FRAMES_FREE = 2, // () -> frames the kernel's one pool has left
    KOS_ASPACE_OP_ROUNDTRIP = 3, // () -> how far map, write, read back, unmap got (0..4)
    // () -> 1 if two virtual pages alias the chosen frame.
    KOS_ASPACE_OP_ALIAS = 4,
    KOS_ASPACE_OP_REFUSALS = 5,  // () -> the KOS_ASPACE_REFUSE_* bits that held
    // () -> frames not returned by create/map/unmap/destroy. Zero means balanced;
    // all ones means an attempted free of an unowned frame.
    KOS_ASPACE_OP_BALANCE = 6,
    // () -> activate a space and read a just-unmapped page; must fault.
    KOS_ASPACE_OP_TOUCH_UNMAPPED = 7,
    // () -> 1 if mapping and unmapping across two table boundaries succeeded.
    KOS_ASPACE_OP_SPAN = 8,
    // () -> stable ID of the caller's address space, or zero if none.
    // Equal IDs mean the same space; the ID is not a kernel address.
    KOS_ASPACE_OP_SPACE_ID = 9,
    // () -> frames not returned after domain resolves/releases; zero means balanced.
    KOS_ASPACE_OP_DOMAIN_BALANCE = 10,
    // () -> remaining reservation slots in the caller's space; slots cannot be freed.
    KOS_ASPACE_OP_RANGES_FREE = 11,
    // (va) -> stable frame ID in the caller's space, or zero if unmapped.
    // IDs count frames from the image's first text page; they are not addresses.
    KOS_ASPACE_OP_FRAME_AT = 12,
    // () -> KOS_ASPACE_SPLIT_* results for adjacent virtual pages backed
    // by nonadjacent frames, built by the kernel probe.
    KOS_ASPACE_OP_SPLIT_ACCESS = 13,
    // (0 or reservation) -> KOS_ASPACE_UNWIND_* results, with allocation-failure
    // injection depth above KOS_ASPACE_UNWIND_DEPTH_SHIFT. Zero tests no-grant
    // creation; a reservation tests creation with a grant.
    KOS_ASPACE_OP_FORCED_UNWIND = 14,
    // () -> number of domains holding an address space.
    KOS_ASPACE_OP_SPACES_HELD = 15,
    // () -> hardware translation report: KOS_ASPACE_MODEL_* flags and widths.
    KOS_ASPACE_OP_MODEL = 16,
    // (va) -> 1 + enum arch_map_memtype in the caller's space, or zero if unmapped.
    // Mappings of the same block must report the same type (see kos_mem_flags).
    KOS_ASPACE_OP_MEMTYPE_AT = 17,
    // () -> outstanding acquires in the high half, unmatched releases in the low
    // half. Both must be zero between calls.
    KOS_ASPACE_OP_ACQUIRE_BALANCE = 18,
    // () -> bit 0: a space-less thread ran; bit 1: reentrancy state was written
    // for it. Expected result: 1.
    KOS_ASPACE_OP_REENT_SEATING = 19,
    // () -> 0 after dropping the space containing the image's original data pages.
    KOS_ASPACE_OP_DATA_HOME_FORGET = 20,
    // () -> issued invalidations in the high half, elided invalidations in the
    // low half, since boot. Never-run spaces need none; previously resident
    // spaces may still have cached translations after switching away.
    KOS_ASPACE_OP_MAP_TLBI = 21,
    // () -> caller-space address of a seeded fresh frame, or zero on setup failure.
    // The caller reads it through its user mapping.
    KOS_ASPACE_OP_MAP_HERE = 22,
    // (readback) -> 1 after validating the seeded word, unmapping MAP_HERE and
    // freeing its frame; zero for a wrong word. The next read must fault.
    KOS_ASPACE_OP_UNMAP_HERE = 23,
    // () -> KOS_ASPACE_DUP_* results for two holds on one page, or zero on setup failure.
    KOS_ASPACE_OP_ACQUIRE_DUP = 24,
    // () -> KOS_ASPACE_CAPOBJ_* results for mint, resolve and close of frame-run
    // and address-space capabilities in the caller's table.
    KOS_ASPACE_OP_CAP_OBJECTS = 25,
    // () -> frame cap in bits 31:0, caller-space cap in bits 63:32, or zero on
    // failure. Test handles for kos_frame_map/unmap; both belong to the caller.
    KOS_ASPACE_OP_CAP_SEED = 26,
    // () -> an unused page-aligned address for mapping the seeded run.
    KOS_ASPACE_OP_CAP_SEED_VA = 27,
    // () -> a capability for the caller's space in its own table, or zero.
    KOS_ASPACE_OP_CAP_SELF_SPACE = 28,
    // () -> holders of the last seeded frame run, including caps and mappings.
    // A mapping must retain frames after the last capability closes.
    KOS_ASPACE_OP_CAP_RUN_REFS = 29,
    // () -> bits 23:16: kernel core count; bits 15:8: cores on the boot root or
    // a live domain root; bits 7:0: cores on the caller's root. The upper two
    // fields must match.
    KOS_ASPACE_OP_ACTIVE_CORES = 30,
    // Values 31..49 are retired ABI slots and must not be reused.
    // () -> peers still using a space at its destruction, since boot. Must be
    // zero: members must leave the space before dropping the last reference.
    KOS_ASPACE_OP_RELEASE_PEER_HITS = 50,
    // () -> space destroys since boot; confirms the peer-hit check actually ran.
    KOS_ASPACE_OP_RELEASE_RUNS = 51
};

// KOS_SYS_AMP_PROBE selectors. Interpret results as signed first to detect
// -KOS_EINVAL; the syscall stub returns an unsigned word.
enum kos_amp_op
{
    // (node) -> send one echo request and ring the peer. Zero means accepted;
    // read KOS_AMP_OP_TOOK later to observe the asynchronous reply.
    KOS_AMP_OP_ROUND = 0,
    // (KOS_AMP_FORGE_* selector) -> inject a malformed inbox publication or
    // exercise send rejection; returns a KOS_AMP_V_* result.
    // Counter ops below take node and return one complete field.
    // Out-of-range nodes return zero.
    KOS_AMP_OP_FORGE = 1,
    KOS_AMP_OP_TOOK = 2,         // messages it took
    KOS_AMP_OP_DEPTH = 3,        // takes refused on the far HEAD's depth
    // Inboxes resynchronized after DEPTH_STRIKES consecutive invalid depths.
    KOS_AMP_OP_DEPTH_RESET = 4,
    KOS_AMP_OP_LENGTH = 5,       // slots refused on the far LENGTH
    KOS_AMP_OP_PORT = 6,         // slots refused on the far PORT
    KOS_AMP_OP_SENT = 7,         // messages it published
    KOS_AMP_OP_SEND_REFUSED = 8, // sends it refused
    KOS_AMP_OP_SERVICED = 9,     // doorbell services that drained its inboxes
    KOS_AMP_OP_REPLY_DROP = 10,  // replies taken and then refused by the tag validation
    // () -> 1 while a thread awaits a remote reply; required by hostile reply tests.
    KOS_AMP_OP_FAR_PARKED = 11,
    // (record) -> 1 if a remote reply handle resolves to a local thread.
    // Must be zero: remote record indices lie outside the local pool.
    KOS_AMP_OP_BAND_RESOLVE = 12,
    // () -> publish with a peer doorbell seat withheld, then restore and drain.
    // Bits 15:0: chosen node; bits 31:16: skipped raises at that node.
    KOS_AMP_OP_DEFER = 13,
    // () -> reset-record checks: 1 abandoned record freed; 2 next call gets a
    // record; 4 token changed; 8 stale token releases no slot; 16 test completed.
    // Zero means the scenario could not run.
    KOS_AMP_OP_RESET_RECORD = 14,
    // (node) -> configured port + 1 after the node's app announces startup;
    // zero before startup or for an out-of-range node. Requires no traffic.
    KOS_AMP_OP_APP_ALIVE = 15,
    // (port) -> announce this node's app startup. Root only. Validate port
    // against the partition's first port for this node; never accept a peer ID.
    // Return 0 or -KOS_EINVAL.
    KOS_AMP_OP_APP_ALIVE_SET = 16,
    // (hold) -> withhold the first peer's doorbell seat, or restore it when zero.
    // Publications remain unserviced while held, keeping remote callers parked.
    // Root only. Return 1 if moved, or 0 when unsupported (skip the test).
    KOS_AMP_OP_PEER_HOLD = 17,
    // (port) -> signed result of privileged endpoint mint for the first peer.
    // Root only. Any minted cap is closed before return. The probe reaches
    // argument checks that unprivileged endpoint creation cannot reach.
    KOS_AMP_OP_MINT = 18,
    // (node) -> calls deferred because the reply ring had no free slot.
    // The call remains unread; this counts backpressure, not lost messages.
    KOS_AMP_OP_REPLY_RESERVE = 19,
    // () -> free reply slots for answering the first peer. Drain first if that
    // peer has no kernel, so earlier forged calls cannot fill the ring forever.
    // Zero means full. Root only.
    KOS_AMP_OP_REPLY_ROOM = 20,
    // (node) -> replies whose content was lost on publication failure or call-ring
    // reset. Count each lost answer once; send an empty reply while the obligation
    // remains valid. Nonzero indicates a malformed or regressed peer.
    KOS_AMP_OP_REPLY_UNSENT = 21,
    // (node) -> reply rings whose head adopted the peer tail after the strike limit.
    KOS_AMP_OP_TAIL_RESET = 22,
    // () -> test the strike limit on the unused self reply ring. Bits: 1 first
    // invalid-tail publication refused; 2 accepted at limit; 4 published at
    // adopted tail; 8 tail_reset incremented; 16 test ran. Root only.
    KOS_AMP_OP_TAIL_RECOVERY = 23,
    // () -> accept a call, remove its reply reservation, then reply to a full
    // ring. Bits: 1 reply_unsent incremented; 2 call slot retained; 4 token dead;
    // 8 test ran; 16 skipped for a live peer. Root only. Finish with ANSWER_DISCHARGE.
    KOS_AMP_OP_ANSWER_DEFER = 24,
    // () -> service the deferred answer. Bits: 1 empty tagged reply sent;
    // 2 call slot returned; 4 no pending record; 8 test ran. Root only.
    KOS_AMP_OP_ANSWER_DISCHARGE = 25,
    // () -> reset a call ring. Bits: 1 reset ran; 2 abandoned record dead;
    // 4 empty tagged reply sent; 8 reply_unsent incremented; 16 test ran;
    // 32 skipped. Root only.
    KOS_AMP_OP_RESET_ANSWERS = 26,
    // (node) -> arrivals answered with -KOS_EFAULT because copying payload or
    // receive-info to a local buffer failed. Count once per arrival; separate
    // from REPLY_UNSENT, which reports malformed peers.
    KOS_AMP_OP_DELIVER_FAULT = 27,
    /* Invalid-op test selector, never dispatched. Keep last so new ops cannot
     * turn the rejection test into a valid request.
     */
    KOS_AMP_OP_MAX
};

/* KOS_AMP_OP_FORGE selectors. Unknown selectors return KOS_AMP_V_EMPTY. */
enum
{
    KOS_AMP_FORGE_WELL_FORMED = 0, // the control: nothing malformed, so it must be taken
    KOS_AMP_FORGE_HEAD_DEPTH = 1,  // the far head names more outstanding slots than the ring
    KOS_AMP_FORGE_LENGTH = 2,      // the far length exceeds one slot
    KOS_AMP_FORGE_PORT = 3,        // the far port names nothing this node minted
    KOS_AMP_FORGE_PORT_WIDE = 4,   // the far port is outside the mint's own width
    KOS_AMP_FORGE_ZERO_LEN = 5,    // a zero-length message, which is one and not an empty ring
    KOS_AMP_FORGE_TAIL_DEPTH = 6,  // the SEND side's half: the tail is the malformed index,
                                   //   forged on the ring nothing drains
    KOS_AMP_FORGE_SELF_SEND = 7,   // a send to the LOCAL node, whose ring nothing drains
    KOS_AMP_FORGE_DEPTH_RESET = 8, // a depth left standing, then a well-formed publication:
                                   // the answer is whether the ring recovered
    /* Inject replies for a caller parked on a remote call. Return KOS_AMP_V_TOOK
     * if delivered or KOS_AMP_V_EMPTY if tag validation drops them.
     */
    KOS_AMP_FORGE_REPLY_UNPARKED = 9,   // a tag for a thread that is not parked at all
    KOS_AMP_FORGE_REPLY_WRONG_RING = 10, // the parked caller's own tag, on another node's ring
    KOS_AMP_FORGE_REPLY_STALE_SEQ = 11,  // the parked caller, one call sequence out of date
    KOS_AMP_FORGE_REPLY_GOOD = 12,       // the control: the right tag on the right ring

    /* Publish a valid peer CALL on this node's first configured port and run
     * the doorbell service. Return TOOK if drained, EMPTY if no port exists.
     * Set KOS_AMP_PEER_CALL_HELD when delivery retains the call slot.
     */
    KOS_AMP_FORGE_PEER_CALL = 13,

    /* Exercise reply-ring strikes while servicing the call ring between strikes.
     * Return TOOK on recovery or DEPTH if still blocked; strike counters must
     * be separate for the two ring classes.
     */
    KOS_AMP_FORGE_REPLY_DEPTH_SERVICE = 14,

    /* Inject a valid peer call with reply-cap write-back failure, as if a parked
     * receiver's output buffer was unmapped. Return the PEER_CALL verdict with
     * HELD clear: an undisclosed capability must not retain the caller's slot.
     */
    KOS_AMP_FORGE_PEER_CALL_BLIND = 15,

    /* Publish a reply-class port into the call ring with valid length and port
     * fields. Return KOS_AMP_V_CLASS.
     */
    KOS_AMP_FORGE_CLASS = 16,

    /* Send a valid empty reply for the parked caller. Return TOOK and wake
     * the caller with zero bytes, as for a call rejected after acceptance.
     */
    KOS_AMP_FORGE_REPLY_EMPTY = 17,

    /* Change only the high byte of the parked caller's sequence. Return EMPTY:
     * validation must compare all 16 bits, even after 256 calls wrap the low byte.
     */
    KOS_AMP_FORGE_REPLY_ALIAS_SEQ = 18,

    /* Leave a valid call unread while its reply ring is full. Return
     * KOS_AMP_V_RESERVE with KOS_AMP_RESERVE_* result bits.
     */
    KOS_AMP_FORGE_RESERVE = 19
};

/* PEER_CALL_HELD means kos_reply must release the retained call slot.
 * It must be clear if reply-cap delivery failed. The verdict occupies
 * the low byte; all verdict values are below 0x100.
 */
/* RESERVE result bits: RAN confirms execution; CURSOR_HELD confirms the
 * call stayed unread; THEN_TOOK confirms delivery after reply space became
 * available. Skip with no bits when a live peer could drain the full ring.
 */
#define KOS_AMP_RESERVE_RAN         0x100u
#define KOS_AMP_RESERVE_CURSOR_HELD 0x200u
#define KOS_AMP_RESERVE_THEN_TOOK   0x400u

#define KOS_AMP_PEER_CALL_HELD 0x100u
#define KOS_AMP_PEER_CALL_VERDICT(x) ((x) & 0xFFu)

enum
{
    KOS_AMP_V_EMPTY = 0,
    KOS_AMP_V_TOOK = 1,
    KOS_AMP_V_DEPTH = 2,
    KOS_AMP_V_LENGTH = 3,
    KOS_AMP_V_PORT = 4,
    KOS_AMP_V_CLASS = 5,   /* a reply on the call ring, or the reverse */
    KOS_AMP_V_RESERVE = 6, /* the call ring holds work the reply ring has no slot to answer */
    KOS_AMP_V_SEND_OK = 16,
    KOS_AMP_V_SEND_DEPTH = 17,
    /* Invalid send length or port. Unlike SEND_FULL, peer draining cannot fix it. */
    KOS_AMP_V_SEND_REFUSED = 18,
    KOS_AMP_V_SEND_NODE = 19,
    /* All ring slots are outstanding; peer draining releases capacity. */
    KOS_AMP_V_SEND_FULL = 20
};

/* KOS_ASPACE_OP_CAP_OBJECTS: capability-check result bits, never frame addresses. */
#define KOS_ASPACE_CAPOBJ_FRAME_MINT    0x01u /* a frame-run capability installed in this table */
#define KOS_ASPACE_CAPOBJ_FRAME_RESOLVE 0x02u /* it resolved back to the run that was minted */
#define KOS_ASPACE_CAPOBJ_ASPACE_MINT   0x04u /* an address-space capability installed */
#define KOS_ASPACE_CAPOBJ_ASPACE_HOLD   0x08u /* minting it took a hold on the domain */
#define KOS_ASPACE_CAPOBJ_ASPACE_STALE  0x10u /* a handle whose slot was reclaimed does NOT resolve */
#define KOS_ASPACE_CAPOBJ_CLOSE_FRAMES  0x20u /* closing the frame cap returned its frames */
#define KOS_ASPACE_CAPOBJ_CLOSE_HOLD    0x40u /* closing the space cap surrendered the hold */
#define KOS_ASPACE_CAPOBJ_BALANCED      0x80u /* the pool and the hold count are back where they began */
#define KOS_ASPACE_CAPOBJ_NO_REFUSED   0x100u /* no refused frees; detects double-free even when pool counts balance */

// KOS_ASPACE_OP_SPLIT_ACCESS: one bit per property of an access split at a page boundary.
#define KOS_ASPACE_SPLIT_NONADJACENT 0x01u /* the two virtually adjacent pages are not physically */
#define KOS_ASPACE_SPLIT_TO_USER     0x02u /* a straddling kaccess_to_user reached both frames */
#define KOS_ASPACE_SPLIT_FROM_USER   0x04u /* a straddling kaccess_from_user read both frames */
#define KOS_ASPACE_SPLIT_CROSS_SPACE 0x08u /* a straddling ep_copy between TWO spaces at ONE address */
#define KOS_ASPACE_SPLIT_NEIGHBOUR   0x10u /* the frame physically after the low page was untouched */
#define KOS_ASPACE_SPLIT_BALANCED    0x20u /* every frame the scenario took came back */
#define KOS_ASPACE_SPLIT_ALL         0x3Fu

// Duplicate-acquire results. Release identifies a hold by (space, page).
#define KOS_ASPACE_DUP_STABLE   0x01u /* two acquires of one page answered one pointer */
#define KOS_ASPACE_DUP_DISTINCT 0x02u /* another page acquired after one release got another */
#define KOS_ASPACE_DUP_REUSABLE 0x04u /* every hold surrendered, the page holdable again */
#define KOS_ASPACE_DUP_ALL      0x07u

// Translation-model checks and hardware-reported widths in bits. Granule
// support uses one bit per architecture-defined size, smallest first.
#define KOS_ASPACE_MODEL_GRANULE     0x01u /* the granule this port programs is supported */
#define KOS_ASPACE_MODEL_ASID        0x02u /* the identifier is as wide as the port's record */
#define KOS_ASPACE_MODEL_PA          0x04u /* the physical range covers what the port programs */
/* The port assigns translation tags. Excluded from ALL because untagged ports are valid. */
#define KOS_ASPACE_MODEL_TAGGED      0x08u
#define KOS_ASPACE_MODEL_ALL         0x07u
#define KOS_ASPACE_MODEL_ASID_SHIFT  8u
#define KOS_ASPACE_MODEL_PA_SHIFT    16u
#define KOS_ASPACE_MODEL_GRAN_SHIFT  24u
#define KOS_ASPACE_MODEL_FIELD_MASK  0xFFu

// KOS_ASPACE_OP_ROUNDTRIP: how far the cycle got, so a failure names its transition.
#define KOS_ASPACE_TRIP_MAPPED    1
#define KOS_ASPACE_TRIP_READBACK  2
#define KOS_ASPACE_TRIP_UNMAPPED  3
#define KOS_ASPACE_TRIP_GONE      4

// KOS_ASPACE_OP_REFUSALS: one bit per refusal the map editor must make.
#define KOS_ASPACE_REFUSE_HIGH_HALF     0x01u /* a kernel-half address */
#define KOS_ASPACE_REFUSE_UNALIGNED     0x02u /* a base below the granule */
#define KOS_ASPACE_REFUSE_EMPTY         0x04u /* zero pages */
#define KOS_ASPACE_REFUSE_NO_READ       0x08u /* write or execute with no read */
#define KOS_ASPACE_REFUSE_UNKNOWN_RIGHT 0x10u /* a bit outside ARCH_MAP_R/W/X */
#define KOS_ASPACE_REFUSE_PART_UNMAP    0x20u /* unmap of a range not wholly mapped */
#define KOS_ASPACE_REFUSE_WRITE_EXEC    0x40u /* writable and executable at once */
#define KOS_ASPACE_REFUSE_PHYS_EXTENT   0x80u /* a run reaching past the output-address width */
#define KOS_ASPACE_REFUSE_PART_MAP     0x100u /* map over a range only partly mapped */
#define KOS_ASPACE_REFUSE_ALL          0x1FFu

// Forced-unwind results. Require nonzero depth to confirm failures were injected.
#define KOS_ASPACE_UNWIND_REFUSED   0x01u /* every injected attempt refused the create */
#define KOS_ASPACE_UNWIND_ENOMEM    0x02u /* and every refusal answered exactly KOS_ENOMEM */
#define KOS_ASPACE_UNWIND_BALANCED  0x04u /* each refusal returned every frame, read at once */
#define KOS_ASPACE_UNWIND_NO_DOUBLE 0x08u /* the pool refused no free over the whole sweep */
#define KOS_ASPACE_UNWIND_SWEPT     0x10u /* the sweep ran past the last allocation a create makes */
#define KOS_ASPACE_UNWIND_REUSABLE  0x20u /* a create after the sweep succeeded and balanced */
#define KOS_ASPACE_UNWIND_ALL       0x3Fu
#define KOS_ASPACE_UNWIND_DEPTH_SHIFT 8
#define KOS_ASPACE_UNWIND_MIN_DEPTH 4

// `op` selector for KOS_SYS_GRANT_PROBE. Ops 0..4 return the predicate as 0/1; ops 5..7
// return a raw count / reserved-block base / size. A BAD op returns -KOS_EINVAL.
enum kos_grant_op
{
    KOS_GRANT_OP_HITS_RESERVED = 0,   // grant_hits_reserved(base, size)
    KOS_GRANT_OP_RAM_PRIVILEGED = 1,  // grant_region_admissible RAM, privileged caller
    KOS_GRANT_OP_RAM_UNPRIVILEGED = 2, // grant_region_admissible RAM, unprivileged caller
    KOS_GRANT_OP_DEV_PRIVILEGED = 3,  // grant_region_admissible DEV, privileged caller
    KOS_GRANT_OP_DEV_UNPRIVILEGED = 4, // grant_region_admissible DEV, unprivileged caller
    KOS_GRANT_OP_RESERVED_COUNT = 5,  // count of arch_reserved_blocks
    KOS_GRANT_OP_RESERVED_BASE = 6,   // reserved block[base].base (base indexes the block)
    KOS_GRANT_OP_RESERVED_SIZE = 7,   // reserved block[base].size (base indexes the block)
    KOS_GRANT_OP_NOCACHE_SUPPORT = 8, // arch_mpu_nocache_support() (enum arch_mpu_nocache)
    KOS_GRANT_OP_RAM_NOCACHE = 9      // grant_region_admissible RAM|NOCACHE, unprivileged
};

#ifdef __cplusplus
}
#endif

#endif
