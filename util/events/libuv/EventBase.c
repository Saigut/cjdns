/* vim: set expandtab ts=4 sw=4: */
/*
 * You may redistribute this program and/or modify it under the terms of
 * the GNU General Public License as published by the Free Software Foundation,
 * either version 3 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */
#include "util/events/libuv/UvWrapper.h"
#include "memory/Allocator.h"
#include "util/events/libuv/EventBase_pvt.h"
#include "util/Assert.h"
#include "util/Identity.h"
#include "util/Js.h"

Js({ require("../util/events/libuv/libuv.js")(builder, js); })

#ifdef win32
    #include <sys/timeb.h>
    #include <time.h>
#else
    #include <sys/time.h>
#endif

static int onFree(struct Allocator_OnFreeJob* job)
{
    struct EventBase_pvt* ctx = Identity_check((struct EventBase_pvt*) job->userData);
    if (ctx->running) {
        // The job will be completed in EventLoop_beginLoop()
        ctx->onFree = job;
        EventBase_endLoop((struct EventBase*) ctx);
        return Allocator_ONFREE_ASYNC;
    } else {
        if (!uv_is_closing((uv_handle_t*) &ctx->blockTimer)) {
            uv_close((uv_handle_t*) &ctx->blockTimer, NULL);
        }
        uv_loop_delete(ctx->loop);
        return 0;
    }
}

static void calibrateTime(struct EventBase_pvt* base)
{
    uint64_t seconds;
    uint64_t milliseconds;

    #ifdef win32
        struct _timeb tb;
        _ftime(&tb);
        seconds = tb.time;
        milliseconds = tb.millitm;
    #else // sane operating systems.
        struct timeval tv;
        gettimeofday(&tv, NULL);
        seconds = tv.tv_sec;
        milliseconds = tv.tv_usec / 1000;
    #endif

    base->baseTime = (seconds * 1000) + milliseconds - uv_now(base->loop);
}

static _Atomic int EventBase_refctr = 0;

struct EventBase* EventBase_new(struct Allocator* allocator)
{
    struct Allocator* alloc = Allocator_child(allocator);
    struct EventBase_pvt* base = Allocator_calloc(alloc, sizeof(struct EventBase_pvt), 1);
    base->loop = uv_loop_new();
    uv_timer_init(base->loop, &base->blockTimer);
    Assert_true(EventBase_refctr == 0);
    base->alloc = alloc;
    Identity_set(base);

    Allocator_onFree(alloc, onFree, base);
    calibrateTime(base);
    return &base->pub;
}

static void doNothing(uv_timer_t* handle, int status)
{
    // title says it all
}

void EventBase_beginLoop(struct EventBase* eventBase)
{
    struct EventBase_pvt* ctx = Identity_check((struct EventBase_pvt*) eventBase);

    Assert_true(!ctx->running); // double begin
    ctx->running = 1;

    do {
        uv_timer_start(&ctx->blockTimer, doNothing, 1, 0);
        // start the loop.
        uv_run(ctx->loop, UV_RUN_DEFAULT);
    } while (EventBase_refctr);

    ctx->running = 0;

    if (ctx->onFree) {
        if (!uv_is_closing((uv_handle_t*) &ctx->blockTimer)) {
            uv_close((uv_handle_t*) &ctx->blockTimer, NULL);
        }
        uv_loop_delete(ctx->loop);
        Allocator_onFreeComplete(ctx->onFree);
        return;
    }
}

void EventBase_endLoop(struct EventBase* eventBase)
{
    struct EventBase_pvt* ctx = Identity_check((struct EventBase_pvt*) eventBase);
    uv_stop(ctx->loop);
}

static void countCallback(uv_handle_t* event, void* vEventCount)
{
    int* eventCount = (int*) vEventCount;
    if (!uv_is_closing(event)) {
        *eventCount = *eventCount + 1;
    }
}

int EventBase_eventCount(struct EventBase* eventBase)
{
    int eventCount = 0;
    struct EventBase_pvt* ctx = Identity_check((struct EventBase_pvt*) eventBase);
    uv_walk(ctx->loop, countCallback, &eventCount);
    return eventCount;
}

struct EventBase_pvt* EventBase_privatize(struct EventBase* base)
{
    return Identity_check((struct EventBase_pvt*) base);
}

void EventBase_ref() {
    EventBase_refctr++;
}
void EventBase_unref() {
    EventBase_refctr--;
}