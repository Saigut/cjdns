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
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#ifndef SessionManager_H
#define SessionManager_H

#include "interface/Interface.h"
#include "net/SessionTable.h"
#include "memory/Allocator.h"
#include "net/Event.h"
#include "net/EventEmitter.h"
#include "wire/SwitchHeader.h"
#include "wire/CryptoHeader.h"
#include "util/Linker.h"
Linker_require("net/SessionManager.c")

/**
 * Purpose of this module is to take packets from "the inside" which contain ipv6 address and
 * skeleton switch header and find an appropriate CryptoAuth session for them or begin one.
 * If a key for this node cannot be found then the packet will be blocked and a search will be
 * triggered. If the skeleton switch header contains "zero" as the switch label, the packet will
 * also be buffered and a search triggered. If a search is in progress (and another packet is
 * already buffered, the packet will be dropped instead).
 * Incoming messages from the outside will be decrypted and their key and path will be stored.
 */
struct SessionManager
{
    /** Sends and handles packets prepped to/from switch. */
    struct Iface switchIf;

    /**
     * Sends and handles packets with RouteHeader on top.
     * When sending a packet to SessionManager:
     *     header.sh.label_be may be zero
     *     version may be zero
     *     publicKey may be zero
     * If these values are not known, the packet will be taken from the cache or a search will
     * be triggered.
     */
    struct Iface insideIf;

    struct SessionTable* sessionTable;

    /**
     * Maximum number of packets to hold in buffer before summarily dropping...
     */
    #define SessionManager_MAX_BUFFERED_MESSAGES_DEFAULT 30
    int maxBufferedMessages;

    /**
     * Number of milliseconds it takes for metric to halve (value of UINT32_MAX - metric halves)
     * This allows less good routes to supplant better ones if the "better" ones have not been
     * tested in a long time (maybe down).
     */
    #define SessionManager_METRIC_HALFLIFE_MILLISECONDS_DEFAULT 250000
    uint32_t metricHalflifeMilliseconds;
};

struct SessionManager* SessionManager_new(struct Allocator* alloc,
                                          struct EventBase* eventBase,
                                          struct CryptoAuth* cryptoAuth,
                                          struct Random* rand,
                                          struct Log* log,
                                          struct EventEmitter* ee);

#endif
