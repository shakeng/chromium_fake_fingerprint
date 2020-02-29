// Copyright 2020 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef THIRD_PARTY_BLINK_RENDERER_PLATFORM_MOJO_HEAP_MOJO_REMOTE_H_
#define THIRD_PARTY_BLINK_RENDERER_PLATFORM_MOJO_HEAP_MOJO_REMOTE_H_

#include "mojo/public/cpp/bindings/remote.h"
#include "third_party/blink/renderer/platform/context_lifecycle_observer.h"
#include "third_party/blink/renderer/platform/heap/heap.h"

namespace blink {

// HeapMojoRemote is a wrapper for mojo::Receiver to be owned by a
// garbage-collected object. Blink is expected to use HeapMojoRemote by
// default. HeapMojoRemote must be associated with context.
// HeapMojoRemote's constructor takes context as a mandatory parameter.
// HeapMojoRemote resets the mojo connection when 1) the owner object is
// garbage-collected and 2) the associated ExecutionContext is detached.
template <typename Interface>
class HeapMojoRemote {
  DISALLOW_NEW();

 public:
  using ImplPointerType = typename mojo::Receiver<Interface>::ImplPointerType;

  HeapMojoRemote(ContextLifecycleNotifier* notifier)
      : wrapper_(MakeGarbageCollected<Wrapper>(notifier)) {}

  // Methods to redirect to mojo::Receiver:
  ImplPointerType operator->() const { return get(); }
  ImplPointerType get() const { return wrapper_->remote().get(); }
  bool is_bound() const { return wrapper_->remote().is_bound(); }
  bool is_connected() const { return wrapper_->remote().is_connected(); }
  void reset() { wrapper_->remote().reset(); }
  void set_disconnect_handler(base::OnceClosure handler) {
    wrapper_->remote().set_disconnect_handler(std::move(handler));
  }
  mojo::PendingReceiver<Interface> BindNewPipeAndPassReceiver(
      scoped_refptr<base::SequencedTaskRunner> task_runner) WARN_UNUSED_RESULT {
    return wrapper_->remote().BindNewPipeAndPassReceiver(
        std::move(task_runner));
  }
  void Bind(mojo::PendingRemote<Interface> pending_remote,
            scoped_refptr<base::SequencedTaskRunner> task_runner) {
    wrapper_->remote().Bind(std::move(pending_remote), std::move(task_runner));
  }

  void Trace(Visitor* visitor) { visitor->Trace(wrapper_); }

 private:
  // Garbage collected wrapper class to add a prefinalizer.
  class Wrapper final : public GarbageCollected<Wrapper>,
                        public ContextLifecycleObserver {
    USING_PRE_FINALIZER(Wrapper, Dispose);
    USING_GARBAGE_COLLECTED_MIXIN(Wrapper);

   public:
    Wrapper(ContextLifecycleNotifier* notifier) {
      SetContextLifecycleNotifier(notifier);
    }

    void Trace(Visitor* visitor) override {
      ContextLifecycleObserver::Trace(visitor);
    }

    void Dispose() { remote_.reset(); }

    mojo::Remote<Interface>& remote() { return remote_; }

    // ContextLifecycleObserver methods
    void ContextDestroyed() override { remote_.reset(); }

   private:
    mojo::Remote<Interface> remote_;
  };

  Member<Wrapper> wrapper_;
};

}  // namespace blink

#endif  // THIRD_PARTY_BLINK_RENDERER_PLATFORM_MOJO_HEAP_MOJO_REMOTE_H_
