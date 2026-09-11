// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The `op` selectors and result encodings of the KOS_SYS_*_PROBE syscalls, whose numbers are
// in <kickos/sys/abi.h> and whose wrappers are in <kickos/sys.h>. Every value below is a
// frozen contract: append, never reorder.
//
// Nothing in <kickos/sys.h> or <kickos/sys/abi.h> includes this, and neither may come to: a
// consumer that never probes must not have to parse it. The extern "C" block below wraps no
// declaration and must stay: it is what puts a header with C consumers and no C-facing
// includer into tests/static/check_c_headers.sh's corpus.

#ifndef KICKOS_SYS_ABI_PROBE_H
#define KICKOS_SYS_ABI_PROBE_H

#ifdef __cplusplus
extern "C"
{
#endif

// `op` selector for KOS_SYS_DOORBELL_PROBE.
enum kos_doorbell_op
{
    KOS_DOORBELL_OP_COUNTS = 0, // (core) -> what `core` has done with the cross-core doorbell,
                                //   two fields in one word:
                                //     63..32  instruction-side rendezvous it INITIATED
                                //     31..0   doorbell services it PERFORMED, each of which
                                //             takes a Context synchronization event
                                //   A core outside the built range reads 0, so a caller may
                                //   sweep a fixed width. EVERY service counts here, a
                                //   cross-core wake included, so the low field is an upper
                                //   bound on the pokes answered
    KOS_DOORBELL_OP_WIDTH = 1,  // () -> how many cores the rendezvous matrix is indexed by,
                                //   which is the MACHINE's core count and not the cores this
                                //   image drives: a sweep bounded by the image's count reads
                                //   one row of several and reports the rest as absent
    KOS_DOORBELL_OP_SELF = 2,   // () -> which row of that matrix the CALLING core writes. Not
                                //   derivable in userspace, and a caller assuming row 0 reads
                                //   a peer's row on every node but the first
    // (from) -> the lowest IRQ line at or above `from` that this arch dispatches to a kernel
    // vector of its own, which no IRQ capability may name; -1 when the arch reserves none at
    // or above it. READ IT SIGNED: the answer arrives in an unsigned word, where -1 is huge.
    KOS_DOORBELL_OP_KERNEL_LINE = 3
};

// `op` selector for KOS_SYS_SCHED_PROBE.
enum kos_sched_op
{
    // The core the caller is running on AT THE MOMENT OF THE READ. Meaningful only for a
    // thread whose affinity is a single bit; anything else may have moved by the time the
    // answer lands, which is the point of asking it of a pinned thread.
    KOS_SCHED_OP_CORE = 0,
    KOS_SCHED_OP_AFFINITY = 1,   // () -> the caller's own core mask
    KOS_SCHED_OP_TASK_CORES = 2, // () -> the caller's task's core set
    KOS_SCHED_OP_CEILING = 3,    // () -> the caller's task's priority ceiling
    KOS_SCHED_OP_ISOLATED = 4,   // () -> the cores this image isolates
    // () -> one bit per core whose OWN slice timer has taken a thread off it since boot; a
    // cross-core reschedule or a device wake sets nothing. Machine-wide and monotonic, so a
    // caller reads a floor.
    KOS_SCHED_OP_PREEMPTED = 5
};

// `op` selector for KOS_SYS_ASPACE_PROBE.
enum kos_aspace_op
{
    KOS_ASPACE_OP_GRANULE = 0,   // () -> the map editor's granule in bytes
    KOS_ASPACE_OP_MEMTYPE = 1,   // (enum arch_map_memtype as a number) -> 1 honoured, 0 not
    KOS_ASPACE_OP_FRAMES_FREE = 2, // () -> frames the kernel's one pool has left
    KOS_ASPACE_OP_ROUNDTRIP = 3, // () -> how far map, write, read back, unmap got (0..4)
    KOS_ASPACE_OP_ALIAS = 4,     // () -> 1 when two unequal virtual pages reached the one
                                 //   frame the caller chose, which an identity map cannot do
    KOS_ASPACE_OP_REFUSALS = 5,  // () -> the KOS_ASPACE_REFUSE_* bits that held
    KOS_ASPACE_OP_BALANCE = 6,   // () -> frames a create/map/unmap/destroy cycle did not
                                 //   return; 0 is balanced, and all ones means the cycle
                                 //   asked the pool to free a frame it does not own
    KOS_ASPACE_OP_TOUCH_UNMAPPED = 7, // () -> activates a space and reads a page it just
                                 //   unmapped, so on a working backend it does not return
    KOS_ASPACE_OP_SPAN = 8,      // () -> 1 when a range crossing two table boundaries mapped
                                 //   contiguously and unmapped whole
    KOS_ASPACE_OP_SPACE_ID = 9,  // () -> a small stable name for the CALLING task's address
                                 //   space, 0 when it holds none. Two tasks answering the
                                 //   same number are one address space; the number is not a
                                 //   kernel address and nothing else may be read out of it
    KOS_ASPACE_OP_DOMAIN_BALANCE = 10, // () -> frames a run of domain resolves and releases
                                 //   did not return; 0 is balanced
    KOS_ASPACE_OP_RANGES_FREE = 11, // () -> range slots the CALLING task's space has left.
                                 //   Allocation spends one and there is no free, so this is
                                 //   how many more blocks the caller may reserve
    KOS_ASPACE_OP_FRAME_AT = 12, // (a virtual address) -> a small stable name for the frame
                                 //   backing it in the CALLING task's space, 0 when the page
                                 //   is not mapped. Two tasks answering DIFFERENT numbers for
                                 //   one address are two copies of it; the number counts frames
                                 //   from the image's first text page, which is no address at
                                 //   all, and nothing else may be read out of it
    KOS_ASPACE_OP_SPLIT_ACCESS = 13, // () -> the KOS_ASPACE_SPLIT_* bits that held for a
                                 //   virtually contiguous range whose two pages are backed by
                                 //   NON-ADJACENT frames, which no caller can itself build
    KOS_ASPACE_OP_FORCED_UNWIND = 14, // (0, or a range this task reserved) -> the
                                 //   KOS_ASPACE_UNWIND_* bits that held, with the number of
                                 //   injection points the sweep reached above bit
                                 //   KOS_ASPACE_UNWIND_DEPTH_SHIFT. A forced allocation failure
                                 //   is walked through the domain-create path one allocation at
                                 //   a time, so every unwind arm under it runs once. At 0 the
                                 //   path is the no-grant create; with a reservation named it is
                                 //   the grant-carrying one, whose handoff unwinds separately
    KOS_ASPACE_OP_SPACES_HELD = 15, // () -> how many domain slots hold an address space at all.
                                 //   Churn that ends with a different answer stranded a root,
                                 //   which a frame delta only implies
    KOS_ASPACE_OP_MODEL = 16,    // () -> what the implementation reports about its own
                                 //   translation, as the KOS_ASPACE_MODEL_* bits and the three
                                 //   widths beside them, which the port's own recorded figures
                                 //   are compared against
    KOS_ASPACE_OP_MEMTYPE_AT = 17, // (a virtual address) -> 1 + the memory TYPE the CALLING
                                 //   task's mapping of it carries (enum arch_map_memtype),
                                 //   or 0 when the page is not mapped. The flag belongs to
                                 //   the block, so two mappings of one block answering
                                 //   differently is the disagreement kos_mem_flags warns of
    KOS_ASPACE_OP_ACQUIRE_BALANCE = 18, // () -> outstanding page acquires in the high half of
                                 //   the word and releases that paired with none in the low
                                 //   half. Both are 0 between calls
    KOS_ASPACE_OP_REENT_SEATING = 19, // () -> bit 0 when a thread whose task holds no space
                                 //   has been switched in, bit 1 when libc's reentrant state
                                 //   was written for such a thread. 1 is the only right answer
    KOS_ASPACE_OP_DATA_HOME_FORGET = 20, // () -> 0, having dropped the space that holds the
                                 //   image's own data pages, as its release would
    KOS_ASPACE_OP_MAP_TLBI = 21, // () -> page-invalidation sequences the map editor has
                                 //   ISSUED in the high half of the word and ELIDED in the
                                 //   low half, both since boot. A space installed on no core
                                 //   caches nothing, so seeding one lands wholly in the low
                                 //   half; a widening of the running space lands in the high
    KOS_ASPACE_OP_MAP_HERE = 22, // () -> a virtual address the CALLING task's own space now
                                 //   maps onto a fresh frame the kernel has seeded, or 0 when
                                 //   the scenario could not be set up. The caller reads that
                                 //   address ITSELF, through the running translation and at
                                 //   the unprivileged level
    KOS_ASPACE_OP_UNMAP_HERE = 23, // (the word the caller read back) -> 1 once the page
                                 //   KOS_ASPACE_OP_MAP_HERE handed out is unmapped from the
                                 //   calling task's space and its frame returned, 0 when the
                                 //   word is not the one the kernel seeded, which means the
                                 //   caller's read did not reach that frame. The caller's next
                                 //   read of the address must fault
    KOS_ASPACE_OP_ACQUIRE_DUP = 24, // () -> the KOS_ASPACE_DUP_* bits that held for two
                                 //   simultaneous acquires of ONE page, or 0 where the
                                 //   scenario could not be built
    KOS_ASPACE_OP_CAP_OBJECTS = 25, // () -> the KOS_ASPACE_CAPOBJ_* bits that held for the
                                 //   two object kinds the capability layer carries, a
                                 //   frame RUN and an address space: minted into the caller's
                                 //   own table, resolved back through the chokepoint, and
                                 //   closed. Every bit answers about a HANDLE and none of
                                 //   them is an address
    KOS_ASPACE_OP_CAP_SEED = 26, // () -> a frame capability in the low 32 bits and an
                                 //   address-space capability naming the CALLER's own space
                                 //   in the high 32, both minted into the caller's table, or
                                 //   0 when either could not be. Scaffolding: there is no
                                 //   user-facing mint yet, and the arm drives the REAL
                                 //   kos_frame_map and kos_frame_unmap syscalls on these
    KOS_ASPACE_OP_CAP_SEED_VA = 27, // () -> a page-aligned virtual address the seeded run may
                                 //   be mapped at, which nothing in the caller's space names
    KOS_ASPACE_OP_CAP_SELF_SPACE = 28, // () -> an address-space capability naming the CALLER's
                                 //   OWN space, minted into its table, or 0. A child task
                                 //   needs one for its own space and holds no other way to
                                 //   name it
    KOS_ASPACE_OP_CAP_RUN_REFS = 29, // () -> how many holders the last seeded frame RUN has,
                                 //   capabilities and MAPPINGS alike. A mapping is a holder:
                                 //   without that the last capability's drop frees frames a
                                 //   live leaf still points at
    KOS_ASPACE_OP_ACTIVE_CORES = 30, // () -> where the kernel's cores stand on translation
                                 //   roots, three fields in one word:
                                 //     23..16  cores one kernel schedules on, the denominator
                                 //     15..8   of those, the ones whose installed root is the
                                 //             boot root or a root some live domain holds
                                 //      7..0   cores the CALLING task's own space is installed
                                 //             on, which is its ACTIVE-CORE SET counted
                                 //   The middle field equalling the high one is the invariant
                                 //   that no core holds a root it is not running
    // 31 and 32 through 49 are SPENT and may not be reused: they answered the shared
    // window's and the doorbell's ops, which hold syscalls of their own. See enum kos_amp_op
    // and enum kos_doorbell_op.
    KOS_ASPACE_OP_RELEASE_PEER_HITS = 50, // () -> peer cores a space destroy has found still
                                 //   holding the space it was destroying, since boot. 0 is the
                                 //   only right answer: a dying member vacates its space
                                 //   before dropping the reference that can destroy it, so the
                                 //   destroy's peer sweep is bookkeeping and never a repair
    KOS_ASPACE_OP_RELEASE_RUNS = 51 // () -> address-space destroys run since boot. The
                                 //   denominator under the counter above, which reads 0 both
                                 //   for a sweep that found nothing and for a destroy that
                                 //   never ran
};

// `op` selector for KOS_SYS_AMP_PROBE.
//
// Every op answers -KOS_EINVAL as a negative value in a register the stub returns UNSIGNED, so
// a caller comparing a counter against zero reads a refusal as a very large count. Read the
// answer as signed before reading it as a number.
enum kos_amp_op
{
    KOS_AMP_OP_ROUND = 0,        // (node) -> drive ONE echo round at `node`: publish a
                                 //   request on the echo port and ring that node's doorbell.
                                 //   Returns the send's own verdict, 0 for accepted; the
                                 //   REPLY arrives later, through this node's own doorbell,
                                 //   so a caller reads KOS_AMP_OP_TOOK for it
    KOS_AMP_OP_FORGE = 1,        // (selector) -> write ONE publication into THIS node's
                                 //   inbox exactly as a far side would and report what the
                                 //   validation made of it, or drive the send side's own
                                 //   refusal. The KOS_AMP_FORGE_* selectors say which
                                 //   malformation, and the KOS_AMP_V_* codes are the answers
                                 // The counter family below: ONE OP PER FIELD of `node`'s
                                 //   window record, each (node) -> that one counter, whole.
                                 //   A node outside the built range reads a ZERO row and not
                                 //   node 0's, so a caller may sweep a fixed width without a
                                 //   real peer's answer standing in for a node that has none
    KOS_AMP_OP_TOOK = 2,         // messages it took
    KOS_AMP_OP_DEPTH = 3,        // takes refused on the far HEAD's depth
    KOS_AMP_OP_DEPTH_RESET = 4,  // inboxes it resynchronised after DEPTH_STRIKES
                                 //   consecutive refused depths, which is what bounds how
                                 //   long a far side may keep one of its rings dead
    KOS_AMP_OP_LENGTH = 5,       // slots refused on the far LENGTH
    KOS_AMP_OP_PORT = 6,         // slots refused on the far PORT
    KOS_AMP_OP_SENT = 7,         // messages it published
    KOS_AMP_OP_SEND_REFUSED = 8, // sends it refused
    KOS_AMP_OP_SERVICED = 9,     // doorbell services that drained its inboxes
    KOS_AMP_OP_REPLY_DROP = 10,  // replies taken and then refused by the tag validation
    KOS_AMP_OP_FAR_PARKED = 11,  // () -> 1 while some thread is parked on a far reply,
                                 //   which is what the hostile-reply forges need to exist
                                 //   before they mean anything
    KOS_AMP_OP_BAND_RESOLVE = 12, // (record) -> 1 where a reply handle naming reply
                                 //   record `record` RESOLVES to a local thread, which it
                                 //   must never do: the band sits above every index the pool
                                 //   can seat, so cap_reply_thread's first clause refuses it
    KOS_AMP_OP_DEFER = 13,       // () -> drive ONE publication at a peer whose seat has been
                                 //   put back to unseated, so the raise is skipped, then seat
                                 //   it again and let it drain. Two fields in one word:
                                 //     15..0   the node it published to
                                 //     31..16  raises skipped at that node
                                 //   The node is reported because the caller cannot derive it:
                                 //   the peer is the kernel's own choice
    KOS_AMP_OP_RESET_RECORD = 14, // () -> the four claims a ring resynchronisation
                                 //   owes the inbound records it abandons, as a bit each:
                                 //   1 the reset freed the record whose slot it abandoned,
                                 //   2 the next call at that masked slot was granted one,
                                 //   4 that record's token is not the abandoned one's,
                                 //   8 spending the abandoned token released no slot.
                                 //   Bit 16 says the scaffold ran to the end, so 0 is a forge
                                 //   that could not run rather than four failed claims.
    KOS_AMP_OP_APP_ALIVE = 15,   // (node) -> the port the partition names `node` biased by
                                 //   one, where that node's APP has declared itself running,
                                 //   and 0 where it has not. THE ONE READING A NODE THIS
                                 //   PARTITION NEVER CALLS CAN BE WITNESSED BY: every counter
                                 //   above needs a crossing. Read as the counter family is,
                                 //   and a node outside the built range answers 0
    KOS_AMP_OP_APP_ALIVE_SET = 16, // (port) -> declare the CALLING node's app running. Root
                                 //   only, and it writes THIS node's own row alone: the node
                                 //   is derived and is never a parameter, so no caller can
                                 //   speak for a peer. `port` is a CLAIM checked against the
                                 //   kernel's own copy of the partition list and refused with
                                 //   -KOS_EINVAL where it is not the first port that list
                                 //   names this node, so what reaches the shared region is the
                                 //   kernel's derivation and never the caller's word.
                                 //   Answers 0
    KOS_AMP_OP_PEER_HOLD = 17,   // (hold) -> withhold the FIRST peer's doorbell seat where
                                 //   `hold` is non-zero, or put back what was there where it
                                 //   is zero. A publication made while the seat is withheld
                                 //   reaches the peer's ring with NO raise behind it, so the
                                 //   peer never services it and a far caller stays parked for
                                 //   as long as the hold lasts: the one way an arm gets a
                                 //   parked far caller now that a serving node answers every
                                 //   call it takes. Root only. Answers 1 where the seat moved
                                 //   and 0 where the backend keeps none, which is a scenario
                                 //   the arm must skip rather than a failure
    KOS_AMP_OP_MINT = 18,        // (port) -> what the privileged far-endpoint mint answers for
                                 //   the FIRST peer at `port`, as a signed rc. The reachable
                                 //   route to that refusal: KOS_SYS_AMP_ENDPOINT_CREATE gates
                                 //   on the caller being privileged and root is unprivileged
                                 //   from its first instruction, so no user thread ever reaches
                                 //   the mint's own argument checks through it. A capability
                                 //   this DOES install is closed here, so the caller's table is
                                 //   as it was whatever the answer. Root only
    KOS_AMP_OP_REPLY_RESERVE = 19, // (node) -> takes `node` refused because its reply ring had
                                 //   no slot to answer with. Apart from send_refused: nothing
                                 //   was published and the call is still unread, so this is
                                 //   back-pressure and not a loss
    KOS_AMP_OP_REPLY_ROOM = 20,  // () -> free slots in the reply ring THIS node answers the
                                 //   first peer's calls into, having first DRAINED it where
                                 //   that peer runs no kernel of its own. An arm that forges a
                                 //   call and expects it taken owes this: every take reserves
                                 //   a slot there, and on a node booted alone nothing else ever
                                 //   moves that tail, so an earlier arm's answers wedge every
                                 //   later take at KOS_AMP_V_RESERVE. Zero means full. Root only
    KOS_AMP_OP_REPLY_UNSENT = 21, // (node) -> answers whose BYTES `node` lost: a publication
                                 //   its reply ring refused, and a record a call-ring
                                 //   resynchronisation abandoned before its service replied.
                                 //   ONE COUNT PER LOST ANSWER and never one per attempt at
                                 //   its refusal, so this counts lost CONTENT: the caller is
                                 //   answered an empty reply unless that obligation outlived
                                 //   its own bound too. Both causes need a malformed or
                                 //   regressed peer, so a non-zero answer names one
    KOS_AMP_OP_TAIL_RESET = 22,  // (node) -> reply rings whose far TAIL `node` stopped
                                 //   believing, its own head resynchronised to that tail after
                                 //   the strike bound. The producer's counterpart of
                                 //   KOS_AMP_OP_DEPTH_RESET, and what says a peer that
                                 //   regressed its reply tail no longer wedges these answers
    KOS_AMP_OP_TAIL_RECOVERY = 23, // () -> drives that bound over this node's SELF reply ring,
                                 //   which no node produces into and no service drains, and
                                 //   answers a bit per claim: 1 the first publication against
                                 //   an incredible tail was refused, 2 the one at the bound was
                                 //   taken, 4 it published AT the adopted tail, 8 tail_reset
                                 //   moved by one; 16 the scaffold ran. Root only
    KOS_AMP_OP_ANSWER_DEFER = 24, // () -> takes and seats one call, withdraws the reservation
                                 //   behind it, and spends the record's reply into a ring with
                                 //   no room. Bits: 1 reply_unsent moved by one, 2 the call
                                 //   slot was NOT released, 4 the token no longer resolves;
                                 //   8 the scaffold ran, 16 it DECLINED against a live peer.
                                 //   Root only, and KOS_AMP_OP_ANSWER_DISCHARGE finishes it
    KOS_AMP_OP_ANSWER_DISCHARGE = 25, // () -> one service pass over what the op above deferred.
                                 //   Bits: 1 an empty reply carrying that record's tag was
                                 //   published, 2 the call slot is back with the peer, 4 no
                                 //   record of the pair is left pending; 8 the scaffold ran.
                                 //   Root only
    KOS_AMP_OP_RESET_ANSWERS = 26, // () -> what a CALL-ring resynchronisation owes the callers
                                 //   it abandons. Bits: 1 the resynchronisation ran, 2 the
                                 //   record it abandoned is dead, 4 an empty reply carrying
                                 //   that record's tag was published, 8 reply_unsent moved by
                                 //   one; 16 the scaffold ran, 32 it DECLINED. Root only
    KOS_AMP_OP_DELIVER_FAULT = 27, // (node) -> arrivals `node` could not copy into a local
                                 //   thread's buffer and answered -KOS_EFAULT for: a reply
                                 //   payload, a call payload, or a call receiver's
                                 //   kos_recv_info. APART from KOS_AMP_OP_REPLY_UNSENT, which
                                 //   names a malformed peer; every cause here is this node's
                                 //   own buffer fault on a message that arrived intact. One
                                 //   count per arrival
    /* NEVER PASSED: the width of the set. An arm proving its reading instrument on an op the
       dispatch does not carry names THIS and not the last op plus one, so adding an op above
       does not silently make that arm assert a real op. STAYS LAST. */
    KOS_AMP_OP_MAX
};

/* KOS_AMP_OP_FORGE: which malformation to write, and what the validation answers with.
   A selector this build does not know reads back KOS_AMP_V_EMPTY. */
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
    /* The four below play a PORT_REPLY at whatever thread is parked on a far call, which is
       the only way an arm reaches the tag validation's bounds. Each answers KOS_AMP_V_TOOK
       where the reply reached that caller and KOS_AMP_V_EMPTY where it was dropped. */
    KOS_AMP_FORGE_REPLY_UNPARKED = 9,   // a tag for a thread that is not parked at all
    KOS_AMP_FORGE_REPLY_WRONG_RING = 10, // the parked caller's own tag, on another node's ring
    KOS_AMP_FORGE_REPLY_STALE_SEQ = 11,  // the parked caller, one call sequence out of date
    KOS_AMP_FORGE_REPLY_GOOD = 12,       // the control: the right tag on the right ring

    /* The peer's own publication, and the only selector that stands in for a NODE rather than
       for a malformation. It publishes one well-formed CALL from the peer on the first port
       the partition names this node, then runs the doorbell's own service body, so what takes
       it is the real dispatch onto the endpoint the partition bound. Answers KOS_AMP_V_TOOK
       where the drain ran and KOS_AMP_V_EMPTY where the partition names this node no port,
       with KOS_AMP_PEER_CALL_HELD set above the verdict where the delivery kept the slot. */
    KOS_AMP_FORGE_PEER_CALL = 13,

    /* The strike bound on the REPLY ring, run through the doorbell's own service body so the
       call ring of the same pair is taken between every pair of reply strikes. Answers
       KOS_AMP_V_TOOK where the reply ring recovered and KOS_AMP_V_DEPTH where it is still
       wedged, which is what a strike count shared between the two classes leaves it. */
    KOS_AMP_FORGE_REPLY_DEPTH_SERVICE = 14,

    /* The same publication, with the delivery's disclosure of the reply capability refused.
       That refusal is forged because no syscall reaches it: the receive proves its out-ptr
       writable before the park, so only a buffer that went away under a parked receiver
       presents it. Answers as KOS_AMP_FORGE_PEER_CALL does, and KOS_AMP_PEER_CALL_HELD must
       be CLEAR: a capability nobody was told of holds the caller's slot for the life of the
       image. */
    KOS_AMP_FORGE_PEER_CALL_BLIND = 15,

    /* A REPLY-class port published into the CALL ring: the one malformation neither the length
       clause nor the port clause can see, since both fields are well-formed. Answers
       KOS_AMP_V_CLASS. */
    KOS_AMP_FORGE_CLASS = 16,

    /* KOS_AMP_FORGE_REPLY_GOOD's tag and ring, carrying NO payload, which is the answer a
       serving node publishes for a call it refused past the take. Answers KOS_AMP_V_TOOK
       where it reached the parked caller, who must wake with a zero-length reply. */
    KOS_AMP_FORGE_REPLY_EMPTY = 17,

    /* The parked caller's tag with its sequence moved ABOVE the low byte alone, which is the
       alias a caller reaches by retrying: 256 short calls bring the low byte round again while
       the caller still holds the tag. Answers KOS_AMP_V_EMPTY, the whole 16-bit sequence being
       what the far arm compares. */
    KOS_AMP_FORGE_REPLY_ALIAS_SEQ = 18,

    /* A well-formed CALL left unread while the reply ring this node would answer it into holds
       no free slot, which is the one refusal naming this node's OWN state rather than an
       untrusted far field. Answers KOS_AMP_V_RESERVE with the KOS_AMP_RESERVE_* bits above. */
    KOS_AMP_FORGE_RESERVE = 19
};

/* KOS_AMP_FORGE_PEER_CALL rides its answer above the verdict: SET where the call's ring slot
   is still the receiver's to release through kos_reply, CLEAR where the taker released it on
   the spot. A receiver that cannot be handed a reply capability must read CLEAR, or its
   caller's slot is held by a capability nobody can spend. Every verdict below is under 0x100,
   so KOS_AMP_PEER_CALL_VERDICT is the whole of the split. */
/* KOS_AMP_FORGE_RESERVE rides three claims above its verdict. RAN separates a forge that
   DECLINED from one whose refusal is the answer: the scenario holds the reply ring toward a peer
   full, which a peer running a kernel of its own would drain out from under the take, so against
   one this forge declines and sets NOTHING rather than answering wrongly. CURSOR_HELD is the
   property that separates RESERVE from every other refusal on that ring, a malformed slot being
   dropped with its cursor advanced where this one is left to be taken. THEN_TOOK is that same
   call taken once the ring had room, so a refusal that LOST the call cannot pass. */
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
    /* A MALFORMED ARGUMENT of the send itself: a length past one slot, or a port outside the
       mint width. Apart from KOS_AMP_V_SEND_FULL below, which the peer's drain clears. */
    KOS_AMP_V_SEND_REFUSED = 18,
    KOS_AMP_V_SEND_NODE = 19,
    /* BACK-PRESSURE: every slot of that ring is outstanding, and the peer's own drain is
       what clears it. */
    KOS_AMP_V_SEND_FULL = 20
};

/* KOS_ASPACE_OP_CAP_OBJECTS: one bit per property of the two capability kinds. The frames a
   run holds are the only thing here a number could leak, and none of these bits is one:
   every bit answers yes or no about a HANDLE. */
#define KOS_ASPACE_CAPOBJ_FRAME_MINT    0x01u /* a frame-run capability installed in this table */
#define KOS_ASPACE_CAPOBJ_FRAME_RESOLVE 0x02u /* it resolved back to the run that was minted */
#define KOS_ASPACE_CAPOBJ_ASPACE_MINT   0x04u /* an address-space capability installed */
#define KOS_ASPACE_CAPOBJ_ASPACE_HOLD   0x08u /* minting it took a hold on the domain */
#define KOS_ASPACE_CAPOBJ_ASPACE_STALE  0x10u /* a handle whose slot was reclaimed does NOT resolve */
#define KOS_ASPACE_CAPOBJ_CLOSE_FRAMES  0x20u /* closing the frame cap returned its frames */
#define KOS_ASPACE_CAPOBJ_CLOSE_HOLD    0x40u /* closing the space cap surrendered the hold */
#define KOS_ASPACE_CAPOBJ_BALANCED      0x80u /* the pool and the hold count are back where they began */
#define KOS_ASPACE_CAPOBJ_NO_REFUSED   0x100u /* the pool refused no free during the probe. A frame
                                                 handed back twice leaves the free COUNT balanced
                                                 and is visible only here */

// KOS_ASPACE_OP_SPLIT_ACCESS: one bit per property of an access split at a page boundary.
#define KOS_ASPACE_SPLIT_NONADJACENT 0x01u /* the two virtually adjacent pages are not physically */
#define KOS_ASPACE_SPLIT_TO_USER     0x02u /* a straddling kaccess_to_user reached both frames */
#define KOS_ASPACE_SPLIT_FROM_USER   0x04u /* a straddling kaccess_from_user read both frames */
#define KOS_ASPACE_SPLIT_CROSS_SPACE 0x08u /* a straddling ep_copy between TWO spaces at ONE address */
#define KOS_ASPACE_SPLIT_NEIGHBOUR   0x10u /* the frame physically after the low page was untouched */
#define KOS_ASPACE_SPLIT_BALANCED    0x20u /* every frame the scenario took came back */
#define KOS_ASPACE_SPLIT_ALL         0x3Fu

// KOS_ASPACE_OP_ACQUIRE_DUP: one bit per property of two simultaneous holds of ONE page. A
// release names (space, page) and nothing else.
#define KOS_ASPACE_DUP_STABLE   0x01u /* two acquires of one page answered one pointer */
#define KOS_ASPACE_DUP_DISTINCT 0x02u /* another page acquired after one release got another */
#define KOS_ASPACE_DUP_REUSABLE 0x04u /* every hold surrendered, the page holdable again */
#define KOS_ASPACE_DUP_ALL      0x07u

// KOS_ASPACE_OP_MODEL: one bit per recorded figure of the port the machine bore out, with the
// figures it reported beside them. The widths are in BITS; the granule field carries one bit per granule
// the architecture defines, smallest first.
#define KOS_ASPACE_MODEL_GRANULE     0x01u /* the granule this port programs is supported */
#define KOS_ASPACE_MODEL_ASID        0x02u /* the identifier is as wide as the port's record */
#define KOS_ASPACE_MODEL_PA          0x04u /* the physical range covers what the port programs */
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
#define KOS_ASPACE_REFUSE_ALL           0xFFu

// KOS_ASPACE_OP_FORCED_UNWIND: one bit per property of the swept forced failure. The DEPTH
// is what stops the whole word passing vacuously: with every bit set and a depth of 0 the
// sweep injected nothing and refused nothing, so an arm reads the depth too.
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
