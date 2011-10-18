#include <string.h>
#include <stdint.h>
#include <event2/event.h>

#ifdef IS_TESTING
    #include <assert.h>
#endif

#include "dht/dhtcore/RouterModule.h"
#include "dht/dhtcore/Node.h"
#include "dht/dhtcore/NodeList.h"
#include "dht/dhtcore/NodeStore.h"
#include "dht/dhtcore/SearchStore.h"
#include "dht/DHTConstants.h"
#include "dht/DHTModules.h"
#include "libbenc/benc.h"
#include "memory/MemAllocator.h"
#include "memory/BufferAllocator.h"
#include "util/AverageRoller.h"
#include "util/Endian.h"
#include "util/Time.h"
#include "util/Timeout.h"

/**
 * The router module is the central part of the DHT engine.
 * It's job is to maintain a routing table which is updated by all incoming packets.
 * When it gets an incoming find_node or get_* request, it's job is to add nodes to the reply
 * so that the asking node can find other nodes which are closer to it's target than us.
 *
 * This implementation does not split nodes explicitly into buckets not does it explicitly try to
 * distinguish between "good" and "bad" nodes. Instead it tries to determine which node will help
 * get to the requested record the fastest. Instead of periodicly pinging a random node in each
 * "bucket", this implementation periodically searches for a random[1] hash. When a node is sent a
 * find_node request, the response time ratio is subtracted from the distance[2] between it and the
 * first node in it's response making a number which represents the node's "reach".
 *
 * The response time ratio is a number ranging between 0 and UINT32_MAX which is a function of the
 * amount of time it takes for a node to respond and the global mean response time.
 * See: calculateResponseTimeRatio() for more information about how it is derived.
 *
 * The global mean response time is the average amount of time it takes a node to respond to a
 * find_node request. It is a rolling average over the past 256 seconds.
 *
 * Visually representing a node as an area whose location is defined by the node id and it's size is
 * defined by the node reach, you can see that there is a possibility for a record to be closer in
 * key space to node2 while is is still further inside of node1's reach thus node1 is a better
 * choice for the next node to ask.
 *
 * |<--------- Node 1 ---------->|
 *                      |<--- Node 2 ---->|
 *                         ^----- Desired record location.
 *
 * Nodes who time out will have a reach set to 0 so bad/dead nodes are ignored but not removed.
 * New nodes are inserted into the table but with a reach of 0. It is up to the search client to
 * send search requests to them so they can prove their validity and have their reach number
 * updated.
 *
 * When a search is carried out, the next k returned nodes are not necessarily the closest known
 * nodes to the id of the record. The nodes returned will be the nodes with the lowest
 * distance:reach ratio. The distance:reach ratio is calculated by dividing the distance between
 * the node and the record by the node's reach number.
 *
 * Since information about a node becomes stale over time, all reach numbers are decreased
 * periodically by a configuration parameter reachDecreasePerSecond times the number of seconds in
 * the last period. Reach numbers which are already equal to 0 are left there.
 *
 * In order to have the nodes with least distance:reach ratio ready to handle any incoming search,
 * we precompute the borders where the "best next node" changes. This computation is best understood
 * by graphing the nodes with their location in keyspace on the X axis and their reach on the Y
 * axis. The border between two nodes, nodeA and nodeB is the location where a line drawn from the
 * X axis up to either node location would be the same angle.
 *
 *  ^                                              ^
 *  |     nodeA                                    |     nodeA
 *  |       |\                                     |       |\__
 *  |       | \                                    |       |   \__
 *  |       |  \    nodeB                          |       |      \nodeB
 *  |       |   \    /|                            |       |         \__
 *  |       |    \  / |                            |       |         |  \__
 *  |       |     \/  |                            |       |         |     \__
 *  +--------------------------------------->      +--------------------------------------->
 *                 ^-- border                                                 ^-- border2
 *
 * Everything to the left of the border and everything to the right of border2 is to be serviced by
 * nodeA. Everything between the two borders is serviced by nodeB. Border2 is found by
 * drawing a line from the point given for nodeA to through the point given for nodeB and finding
 * the intersection of that line with the Y axis. border and border2 are shown on different graphs
 * only to limit clutter, they are the same nodeA and nodeB.
 *
 * When resolving a search, this implementation will lookup the location of the searched for record
 * and return the nodes which belong to the insides of the nearest 8 borders, this guarantees return
 * of the nodes whose distance:reach ratio is the lowest for that location.
 *
 * This implementation must never respond to a search by sending any node who's id is not closer
 * to the target than it's own. Such an event would lead to the possibility of "routing loops" and
 * must be prevented. This node's "opinion of it's own reach" is defined as equal to the reach of
 * the longest reaching node which it knows. Searches forwhich this node has the lowest
 * distance:reach ratio will be replied to with nodes which have 0 reach but are closer than this
 * node or, if there are no such nodes, no nodes at all.
 *
 * The search consumer in this routing module tries to minimize the amount of traffic sent when
 * doing a lookup. To achieve this, it sends a request only to the first node in the search response
 * packet, after the global mean response time has passed without it getting a response, it sends
 * requests to the second third and forth nodes. If after the global mean response time has passed
 * again and it still has not gotten any responses, it will finally send requests to the fifth,
 * sixth, seventh, and eighth nodes.
 *
 * In order to minimize the number of searches which must be replied to with 0 reach nodes because
 * this node is the closest non 0 reach node to the record, this implementation runs periodic
 * searches for random locations where it is the node with the lowest distance:reach ratio.
 * These searches are run periodicly every number of seconds given by the configuration parameter
 * localMaintainenceSearchPeriod.
 *
 * To maximize the quality of service offered by this node and to give other nodes who have 0 reach
 * a chance to prove that they can handle searches, this implementation will repeat searches which
 * it handles every number of seconds given by the configuration parameter
 * globalMaintainenceSearchPeriod.
 *
 * A node which has not responded to a search request in a number of seconds given by the
 * configuration parameter searchTimeoutSeconds will have it's reach set to 0. If a node does this
 * a number of times in a row given by the configuration parameter maxTimeouts, it will be removed
 * from the routing table entirely.
 *
 * [1] The implementation runs periodic searches for random hashes but unless the search target
 *     falls within it's own reach footprint (where this node has the lowest distance:reach ratio)
 *     the search is not preformed. This means that the node will send out lots of searches early
 *     on when it is training in the network but as it begins to know other nodes with reach,
 *     the contrived searches taper off.
 *
 * [2] If a response "overshoots" the record requested then it is calculated as if it had undershot
 *     by the same amount so as not to provide arbitrage advantage to nodes who return results which
 *     are very far away yet very inaccurate. If it overshoots by more than the distance between the
 *     node and the searched for location (this should never happen), it is considered to be 0.
 */

/*--------------------Constants--------------------*/

/** The number of seconds of time overwhich to calculate the global mean response time. */
#define GMRT_SECONDS 256

/**
 * The number to initialize the global mean response time averager with so that it will
 * return sane results
 */
#define GMRT_INITAL_MILLISECONDS 100

/** The number of nodes which we will keep track of. */
#define NODE_STORE_SIZE 16384

/** The number of nodes to return in a search query. */
#define RETURN_SIZE 8


/*--------------------Structures--------------------*/

/** The context for this module. */
struct RouterModule
{
    /** A bencoded string with this node's address tag. */
    String* myAddress;

    /** An AverageRoller for calculating the global mean response time. */
    void* gmrtRoller;

    struct SearchStore* searchStore;

    struct NodeStore* nodeStore;

    /** The registry which is needed so that we can send messages. */
    struct DHTModuleRegistry* registry;

    /** The libevent event base for handling timeouts. */
    struct event_base* eventBase;
};

/*--------------------Prototypes--------------------*/
static int handleIncoming(struct DHTMessage* message, void* vcontext);
static int handleOutgoing(struct DHTMessage* message, void* vcontext);

/*--------------------Interface--------------------*/

/**
 * Register a new RouterModule.
 *
 * @param registry the DHT module registry for signal handling.
 * @param allocator a means to allocate memory.
 * @param myAddress the address for this DHT node.
 * @return the RouterModule.
 */
struct RouterModule* RouterModule_register(struct DHTModuleRegistry* registry,
                                           struct MemAllocator* allocator,
                                           const uint8_t myAddress[20],
                                           struct event_base* eventBase)
{
    struct RouterModule* const out = allocator->malloc(sizeof(struct RouterModule), allocator);

    DHTModules_register(allocator->clone(sizeof(struct DHTModule), allocator, &(struct DHTModule) {
        .name = "RouterModule",
        .context = out,
        .handleIncoming = handleIncoming,
        .handleOutgoing = handleOutgoing
    }), registry);

    out->myAddress = benc_newBinaryString((const char*) myAddress, 20, allocator);
    out->gmrtRoller = AverageRoller_new(GMRT_SECONDS, allocator);
    AverageRoller_update(out->gmrtRoller, GMRT_INITAL_MILLISECONDS);
    out->searchStore = SearchStore_new(allocator);
    out->nodeStore = NodeStore_new(myAddress, NODE_STORE_SIZE, allocator);
    out->registry = registry;
    out->eventBase = eventBase;
    return out;
}

/**
 * Calculate the response time ratio for a given response time.
 * This function also updates the global mean response time.
 *
 * @param gmrtRoller the AverageRoller for the global mean response time.
 * @param responseTime the response time from the node.
 * @return an integer between 0 and UINT32_MAX which represents the distance be node's response
 *         time and the global mean response time. If the node takes twice the global mean or
 *         longer, the number returned is UINT32_MAX. If the response time is equal to the global
 *         mean then the number returned is half of UINT32_MAX and if the response time is 0
 *         then 0 is returned.
 */
static uint32_t calculateResponseTimeRatio(void* gmrtRoller, const uint32_t responseTime)
{
    const uint32_t gmrt = AverageRoller_update(gmrtRoller, responseTime);
    return (responseTime > 2 * gmrt) ? UINT32_MAX : ((UINT32_MAX / 2) / gmrt) * responseTime;
}

/**
 * Calculate "how far this node got us" in our quest for a given record.
 *
 * When we ask node Alice a search query to find a record,
 * if she replies with a node which is further from the target than her, we are backpeddling,
 * Alice is not compliant and we will return 0 distance because her reach should become zero asap.
 *
 * If Alice responds with a node which is further from her than she is from the target, then she
 * has "overshot the target" so to speak, we return the distance between her and the node minus
 * the distance between the node and the target.
 *
 * If alice returns a node which is between her and the target, we just return the distance between
 * her and the node.
 *
 * @param nodeIdPrefix the first 4 bytes of Alice's node id in host order.
 * @param targetPrefix the first 4 bytes of the target id in host order.
 * @param firstResponseIdPrefix the first 4 bytes of the id of
 *                              the first node to respond in host order.
 * @return a number between 0 and UINT32_MAX representing the distance in keyspace which this
 *         node has helped us along.
 */
static inline uint32_t calculateDistance(const uint32_t nodeIdPrefix,
                                         const uint32_t targetPrefix,
                                         const uint32_t firstResponseIdPrefix)
{
    // Distance between Alice and the target.
    uint32_t at = nodeIdPrefix ^ targetPrefix;

    // Distance between Bob and the target.
    uint32_t bt = firstResponseIdPrefix ^ targetPrefix;

    if (bt > at) {
        // Alice is giving us nodes which are further from the target than her :(
        return 0;
    }

    // Distance between Alice and Bob.
    uint32_t ab = nodeIdPrefix ^ firstResponseIdPrefix;

    if (at < ab) {
        // Alice gave us a node which is beyond the target,
        // this is fine but should not be unjustly rewarded.
        return ab - bt;
    }

    // Alice gave us a node which is between her and the target.
    return ab;
}

static inline void cleanup(struct SearchStore* store,
                           struct SearchStore_Node* lastNode,
                           uint8_t targetAddress[20],
                           struct RouterModule* module)
{
    struct SearchStore_Search* search = SearchStore_getSearchForNode(lastNode, store);

    // Add a fake node to the search for the target,
    // this allows us to track the amount of time it took for the last node to get us
    // the result and adjust it's reach accordingly.
    SearchStore_addNodeToSearch(lastNode,
                                targetAddress,
                                (uint8_t*) "Unused",
                                evictUnrepliedIfOlderThan(module),
                                search);

    struct SearchStore_TraceElement* child = SearchStore_backTrace(lastNode, store);
    struct SearchStore_TraceElement* parent = child->next;
    const uint32_t targetPrefix = AddrPrefix_get(targetAddress);
    uint32_t milliseconds = 0;
    while (parent != NULL) {
        struct Node* parentNode = NodeStore_getNode(module->nodeStore, parent->address);
        if (parentNode != NULL) {
            // If parentNode is NULL then it must have been replaced in the node store.
            milliseconds
            parentNode->reach += targetPrefix ^ AddrPrefix_get(child->address) / milliseconds;
            uint32_t time = 
calculateDistance(AddrPrefix_get(parent->address),
                                                  targetPrefix,
                                                  AddrPrefix_get(child->address));
            uint32_t time = calculateResponseTimeRatio(module->gmrtRoller, parent->delayUntilReply);
            parentNode->reach += 
        }

        child = parent;
        parent = parent->next;
    }
}

/**
 * Get the time where any unreplied requests older than that should be timed out.
 * This implementation times out after twice the global mean response time.
 *
 * @param module this module.
 * @return the timeout time.
 */
static inline uint64_t evictUnrepliedIfOlderThan(struct RouterModule* module)
{
    return
        Time_currentTimeMilliseconds() - (((uint64_t) AverageRoller_getAverage(module->gmrtRoller)) * 2);
}

/**
 * The amount of time to wait before skipping over the first node and trying another in a search.
 *
 * @param module this module.
 * @return the timeout time.
 */
static inline uint64_t tryNextNodeAfter(struct RouterModule* module)
{
    return ((uint64_t) AverageRoller_getAverage(module->gmrtRoller)) * 2;
}

/**
 * Send off a query to another node.
 *
 * @param networkAddress the address to send the query to.
 * @param queryType what type of query eg: find_node or get_peers.
 * @param transactionId the tid to send with the query.
 * @param searchTarget the thing which we are looking for or null if it's a ping.
 * @param targetKey the key underwhich to send the target eg: target or info_hash
 * @param module the router module to send the search with.
 */
static void sendRequest(uint8_t networkAddress[6],
                        String* queryType,
                        String* transactionId,
                        String* searchTarget,
                        String* targetKey,
                        struct RouterModule* module)
{
    struct DHTMessage message;
    memset(&message, 0, sizeof(struct DHTMessage));

    char buffer[4096];
    const struct MemAllocator* allocator = BufferAllocator_new(buffer, 4096);

    message.allocator = allocator;
    message.asDict = benc_newDictionary(allocator);

    // "t":"1234"
    benc_putString(message.asDict, &DHTConstants_transactionId, transactionId, allocator);

    // "y":"q"
    benc_putString(message.asDict,
                   &DHTConstants_messageType,
                   &DHTConstants_query,
                   allocator);

    /* "a" : { ...... */
    Dict* args = benc_newDictionary(allocator);
    benc_putString(args, &DHTConstants_myId, module->myAddress, allocator);
    if (searchTarget != NULL) {
        // Otherwise we're sending a ping.
        benc_putString(args, targetKey, searchTarget, allocator);
    }
    benc_putDictionary(message.asDict, &DHTConstants_arguments, args, allocator);

    /* "q":"find_node" */
    benc_putString(message.asDict, &DHTConstants_query, queryType, allocator);

    memcpy(message.peerAddress, networkAddress, 6);
    message.addressLength = 6;

    DHTModules_handleOutgoing(&message, module->registry);
}


struct SearchCallbackContext
{
    struct RouterModule* const routerModule;

    int32_t (* const resultCallback)(void* callbackContext, struct DHTMessage* result);

    void* const resultCallbackContext;

    String* const target;

    struct Timeout* const timeout;

    struct SearchStore_Search* search;

    String* const requestType;
};

static void searchStep(void* vcontext)
{
    struct SearchCallbackContext* context = (struct SearchCallbackContext*) vcontext;
    struct RouterModule* module = context->routerModule;
    struct MemAllocator* searchAllocator = SearchStore_getAllocator(context->search);
    struct SearchStore_Node* nextSearchNode = SearchStore_getNextNode(context->search, searchAllocator);

    sendRequest(nextSearchNode->networkAddress,
                context->requestType,
                SearchStore_tidForNode(nextSearchNode, searchAllocator),
                context->target,
                &DHTConstants_infoHash,
                context->routerModule);

    SearchStore_requestSent(nextSearchNode, module->searchStore);
    Timeout_resetTimeout(context->timeout, tryNextNodeAfter(module));
}

static inline int handleReply(struct DHTMessage* message, struct RouterModule* module)
{
    Dict* arguments = benc_lookupDictionary(message->asDict, &DHTConstants_reply);
    if (arguments == NULL) {
        return 0;
    }
    String* nodes = benc_lookupString(arguments, &DHTConstants_nodes);
    if (nodes == NULL || nodes->len % 26 != 0) {
        // this implementation only pings to get the address of a node, so lets add the node.
        String* address = benc_lookupString(arguments, &DHTConstants_myId);
        NodeStore_addNode(module->nodeStore, (uint8_t*) address->bytes, (uint8_t*) message->peerAddress);
        return -1;
    }

    String* tid = benc_lookupString(message->asDict, &DHTConstants_transactionId);
    struct SearchStore_Node* parent =
        SearchStore_getNode(tid, module->searchStore, message->allocator);

    if (parent == NULL) {
        // Couldn't find the node, perhaps we were sent a malformed packet.
        return -1;
    }

    struct SearchStore_Search* search = SearchStore_getSearchForNode(parent, module->searchStore);
    uint64_t evictTime = evictUnrepliedIfOlderThan(module);

    for (uint32_t i = 0; i < nodes->len; i += 26) {
        NodeStore_addNode(module->nodeStore,
                          (uint8_t*) &nodes->bytes[i],
                          (uint8_t*) &nodes->bytes[i + 20]);
        SearchStore_addNodeToSearch(parent,
                                    (uint8_t*) &nodes->bytes[i],
                                    (uint8_t*) &nodes->bytes[i + 20],
                                    evictTime,
                                    search);
    }

    // Ask the callback if we should continue.
    struct SearchCallbackContext* scc = SearchStore_getContext(search);
    if (!scc->resultCallback(scc->resultCallbackContext, message)) {
        searchStep(SearchStore_getContext(search));
    } else {
        cleanup(module->searchStore, parent, (uint8_t*) scc->target->bytes, module);
    }
    return 0;
}

static int handleIncoming(struct DHTMessage* message, void* vcontext)
{
    String* messageType = benc_lookupString(message->asDict, &DHTConstants_messageType);

    if (benc_stringEquals(messageType, &DHTConstants_reply)) {
        return handleReply(message, (struct RouterModule*) vcontext);
    } else {
        return 0;
    }    
}

static inline int handleQuery(struct DHTMessage* message,
                              Dict* replyArgs,
                              struct RouterModule* module)
{
    struct DHTMessage* query = message->replyTo;

    Dict* queryArgs = benc_lookupDictionary(query->asDict, &DHTConstants_arguments);

    // Add the node to the store.
    String* address = benc_lookupString(queryArgs, &DHTConstants_myId);
    if (address == NULL || address->len != 20) {
        return 0;
    }
    NodeStore_addNode(module->nodeStore, (uint8_t*) address->bytes, (uint8_t*) query->peerAddress);

    // get the target
    String* target = benc_lookupString(queryArgs, &DHTConstants_targetId);
    if (target == NULL) {
        target = benc_lookupString(queryArgs, &DHTConstants_infoHash);
    }
    if (target == NULL || target->len != 20) {
        return 0;
    }

    // send the closest nodes
    struct NodeList* nodeList = NodeStore_getClosestNodes(module->nodeStore,
                                                          (uint8_t*) target->bytes,
                                                          RouterModule_K,
                                                          message->allocator);

    String* nodes = message->allocator->malloc(sizeof(String), message->allocator);
    nodes->len = nodeList->size * 26;
    nodes->bytes = message->allocator->malloc(nodeList->size * 26, message->allocator);

    uint32_t i;
    for (i = 0; i < nodeList->size; i++) {
        memcpy(&nodes->bytes[i * 26], &nodeList->nodes[i]->address, 20);
        memcpy(&nodes->bytes[i * 26 + 20], &nodeList->nodes[i]->networkAddress, 6);
    }
    if (i > 0) {
        benc_putString(replyArgs, &DHTConstants_nodes, nodes, message->allocator);
    }

    return 0;
}

/**
 * We handle 2 kinds of packets on the outgoing.
 * 1. our requests
 * 2. our replies to others' requests.
 * Everything is tagged with our address, replies to requests which are not ping requests
 * will also be given a list of nodes.
 */
static int handleOutgoing(struct DHTMessage* message, void* vcontext)
{
    struct RouterModule* module = (struct RouterModule*) vcontext;

    // Get the key which the arguments section is under (either "r" or "a")
    String* argumentsKey;
    if (message->replyTo != NULL) {
        argumentsKey = &DHTConstants_reply;
    } else {
        argumentsKey = &DHTConstants_arguments;
    }

    Dict* arguments = benc_lookupDictionary(message->asDict, argumentsKey);
    if (arguments == NULL) {
        arguments = benc_newDictionary(message->allocator);
        benc_putDictionary(message->asDict, argumentsKey, arguments, message->allocator);
    }

    benc_putString(arguments, &DHTConstants_myId, module->myAddress, message->allocator);

    if (message->replyTo != NULL) {
        return handleQuery(message, arguments, module);
    }

    return 0;
}

/**
 * Start a search.
 *
 * @param requestType the type of request to send EG: "find_node" or "get_peers".
 * @param searchTarget the address to look for.
 * @param callback the function to call when results come in for this search.
 * @param callbackContext a pointer which will be passed back to the RouterModule_SearchCallback
 *                        when it is called.
 * @param module the router module which should perform the search.
 * @return 0 if all goes well, -1 if the search could not be completed because there are no nodes
 *         closer to the destination than us.
 */
int32_t RouterModule_beginSearch(String* requestType,
                                 const uint8_t searchTarget[20],
                                 int32_t (* const callback)(void* callbackContext,
                                                            struct DHTMessage* result),
                                 void* callbackContext,
                                 struct RouterModule* module)
{
    struct SearchStore_Search* search = SearchStore_newSearch(searchTarget, module->searchStore);
    struct MemAllocator* searchAllocator = SearchStore_getAllocator(search);
    struct NodeList* nodes =
        NodeStore_getClosestNodes(module->nodeStore, searchTarget, RETURN_SIZE, searchAllocator);

    if (nodes->size == 0) {
        // no nodes found!
        return -1;
    }

    for (uint32_t i = 0; i < nodes->size; i++) {
        SearchStore_addNodeToSearch(NULL,
                                    nodes->nodes[i]->address,
                                    nodes->nodes[i]->networkAddress,
                                    evictUnrepliedIfOlderThan(module),
                                    search);
    }

    struct SearchStore_Node* firstSearchNode = SearchStore_getNextNode(search, searchAllocator);

    String* searchTargetString = benc_newBinaryString((char*) searchTarget, 20, searchAllocator);

    // Send out the request.
    sendRequest(firstSearchNode->networkAddress,
                requestType,
                SearchStore_tidForNode(firstSearchNode, searchAllocator),
                searchTargetString,
                &DHTConstants_infoHash,
                module);
    
    SearchStore_requestSent(firstSearchNode, module->searchStore);

    struct SearchCallbackContext* scc =
        searchAllocator->malloc(sizeof(struct SearchCallbackContext), searchAllocator);

    struct Timeout* timeout = Timeout_setTimeout(searchStep,
                                                 scc,
                                                 tryNextNodeAfter(module),
                                                 module->eventBase,
                                                 searchAllocator);

    struct SearchCallbackContext sccLocal = {
        .routerModule = module,
        .resultCallback = callback,
        .resultCallbackContext = callbackContext,
        .target = searchTargetString,
        .timeout = timeout,
        .search = search,
        .requestType = benc_newBinaryString(requestType->bytes, requestType->len, searchAllocator)
    };
    memcpy(scc, &sccLocal, sizeof(struct SearchCallbackContext));

    SearchStore_setContext(scc, search);

    return 0;
}

/** See: RouterModule.h */
void RouterModule_addNode(const uint8_t address[20],
                          const uint8_t networkAddress[6],
                          struct RouterModule* module)
{
    NodeStore_addNode(module->nodeStore, address, networkAddress);
}
