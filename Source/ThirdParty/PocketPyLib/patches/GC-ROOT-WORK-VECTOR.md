# GC root/work vector ownership

`ManagedHeap.gc_roots` is the collector's reusable traversal work vector.  It
is VM-owned infrastructure, rather than an object owned by the script that
happened to trigger collection.  Its backing storage is admitted while that
script's quota cell is active so the VM cap is checked and a denial records the
triggering instance/generation.  Before the vector is published, the cell is
detached from the allocation header: the VM charge remains, but the reusable
vector cannot retain or debit a retired instance cell.

Every work-vector expansion allocates a replacement and copies queued entries
only after admission succeeds.  A denial therefore leaves its pointer, length,
capacity and queued entries unchanged.  Marking propagates that failure to the
collector, clears only provisional mark bits, empties the queue, and returns
without sweeping.  The next unarmed collection starts from a clean mark state.

`py_mantle_gc_root_storage_test` is diagnostics-only coverage for a full
root-vector replacement boundary. It separately denies quota admission and the
root backing allocator after admission, checks the retained pointer/capacity/
entries plus exact terminal/accounting state, then verifies a detached healthy
replacement on retry. The older pre-reserve marker is deliberately not armed.
