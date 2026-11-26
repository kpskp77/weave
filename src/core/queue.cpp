// Communication primitive: Queue exception key functions
//
// Queue<T> itself is a template and therefore header-only (see include/weave/queue.hpp). This TU
// exists to emit the key functions of the three exception types: the out-of-line destructor pins
// the vtable/typeinfo of QueueEmpty / QueueFull / QueueShutDown to exactly one definition inside
// libweave.so, so every consumer TU keeps only an undefined reference and a cross-DSO catch (the
// -sv_lib boundary, the examples linking libweave.so) matches by typeinfo pointer. Written back
// as inline, typeinfo degrades into one weak definition per TU and libc++abi's pointer comparison
// makes catch silently mismatch — not even the linker would complain. Same ABI requirement as
// BrokenBarrierError::~BrokenBarrierError (src/core/barrier.cpp); see the "ABI notes" section of
// docs/backends.md.

#include <weave/queue.hpp>

namespace weave {

// Key function (see barrier.cpp): emitted here, exactly once, so every consumer TU keeps only an
// undefined reference and a cross-DSO catch matches by typeinfo pointer.
QueueEmpty::~QueueEmpty() = default;
QueueFull::~QueueFull() = default;
QueueShutDown::~QueueShutDown() = default;

}  // namespace weave
