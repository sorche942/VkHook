// Drives the two pure pieces behind the Wayland capture subsurface without a
// compositor:
//   - CaptureAttachment: which parent surface the capture is bound to, and the
//     step that re-binds it when the game recreates its window.
//   - ReleaseStack: objects are destroyed in reverse creation order, so no proxy
//     outlives the parent it was created from or the event queue it hangs on.
#include "capture_attachment.hh"
#include "release_stack.hh"

#include <cstdio>
#include <string>
#include <vector>

namespace
{
    int failures = 0;

    void check(bool ok, const char* what)
    {
        std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
        if (!ok)
            ++failures;
    }

    using VkHook::CaptureAttachment;
    using Step = CaptureAttachment::Step;

    // Plays the part of WaylandInput::reconcileCapture against a recording fake.
    struct FakeCapture
    {
        std::vector<std::string> calls;
        bool failCreate = false;
        uintptr_t live = 0;

        Step reconcile(CaptureAttachment& attachment)
        {
            const Step step = attachment.next();
            if (step == Step::None)
                return step;
            if (step == Step::Detach || step == Step::Reattach)
            {
                calls.push_back("destroy " + std::to_string(live));
                live = 0;
            }
            bool ok = true;
            if (step == Step::Attach || step == Step::Reattach)
            {
                ok = !failCreate;
                if (ok)
                {
                    calls.push_back("create " + std::to_string(attachment.parent()));
                    live = attachment.parent();
                }
            }
            attachment.completed(step, ok);
            return step;
        }
    };

    void attachmentTests()
    {
        CaptureAttachment a;
        FakeCapture fake;

        check(a.next() == Step::None, "no parent, no capture: nothing to do");

        a.setParent(10);
        check(a.next() == Step::Attach, "a parent appears: attach");
        fake.reconcile(a);
        check(a.bound() == 10 && fake.live == 10, "attached to the parent");
        check(a.takeRepublish(), "a fresh capture asks for its hitboxes to be republished");
        check(!a.takeRepublish(), "republish is reported once");
        check(a.next() == Step::None, "unchanged parent: nothing to do");

        a.setParent(10);
        check(a.next() == Step::None, "re-announcing the same parent is a no-op");

        a.setParent(20);
        check(a.next() == Step::Reattach, "the game recreated its surface: reattach");
        fake.reconcile(a);
        check(fake.calls.size() == 3 && fake.calls[1] == "destroy 10" && fake.calls[2] == "create 20",
              "old capture destroyed before the new one is created");
        check(a.bound() == 20 && a.takeRepublish(), "bound to the new parent, hitboxes republished");

        a.setParent(0);
        check(a.next() == Step::Detach, "the surface is gone: detach");
        fake.reconcile(a);
        check(a.bound() == 0 && fake.live == 0, "detached");
        check(!a.takeRepublish(), "detach does not ask for a republish");

        a.setParent(30);
        fake.failCreate = true;
        check(fake.reconcile(a) == Step::Attach && a.bound() == 0, "a failed attach leaves no capture");
        check(a.next() == Step::Attach && !a.takeRepublish(), "and is retried, with nothing to republish");
        fake.failCreate = false;
        fake.reconcile(a);
        check(a.bound() == 30 && a.takeRepublish(), "the retry succeeds");

        fake.failCreate = true;
        a.setParent(40);
        check(fake.reconcile(a) == Step::Reattach && fake.live == 0 && a.bound() == 0,
              "a failed reattach has already destroyed the old capture");
        check(a.next() == Step::Attach, "so the retry is a plain attach");
    }

    void releaseOrderTests()
    {
        std::vector<std::string> log;
        auto record = [&log](const char* name) {
            return [&log, name] { log.push_back(name); };
        };

        // the creation order WaylandInput::ensureCommon and the bind callbacks produce
        {
            VkHook::ReleaseStack stack;
            stack.push(record("queue"));
            stack.push(record("wrapper"));
            stack.push(record("registry"));
            stack.push(record("seat"));
            stack.push(record("compositor"));
            stack.push(record("capture"));
            stack.push(record("keyboard"));
            stack.push(record("pointer"));
            stack.releaseAll();

            const std::vector<std::string> expected = {"pointer", "keyboard", "capture", "compositor",
                                                       "seat", "registry", "wrapper", "queue"};
            check(log == expected, "released newest first: devices, then globals, registry, wrapper, queue last");
            check(stack.empty(), "stack is empty afterwards");
        }

        log.clear();
        {
            VkHook::ReleaseStack stack;
            stack.push(record("queue"));
            stack.push(record("wrapper"));
            stack.releaseAll();
            stack.releaseAll();
            check(log.size() == 2 && log[0] == "wrapper" && log[1] == "queue",
                  "a second releaseAll does nothing (idempotent shutdown)");
        }

        // a release may push nothing and may be pushed again after a full release
        log.clear();
        {
            VkHook::ReleaseStack stack;
            stack.push(record("queue"));
            stack.releaseAll();
            stack.push(record("queue2"));
            stack.push(record("wrapper2"));
            stack.releaseAll();
            check(log.size() == 3 && log[1] == "wrapper2" && log[2] == "queue2",
                  "re-initialisation after shutdown unwinds in its own reverse order");
        }
    }
} // namespace

int main()
{
    attachmentTests();
    releaseOrderTests();
    std::printf("%d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
